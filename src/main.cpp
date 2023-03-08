#include "nlohmann/json.hpp"

#include "helpers.hpp"
#include "ArgParser.hpp"
#include "dispatchqueue.hpp"
#include <pigpio.h>
#include <atomic>
#include <chrono>
#include <random>
#include <iostream>
#include <memory>
#include <thread>
#include <unordered_map>
#include <memory>
#include "h264_common.h"
extern "C" {
    #include "mmalcam.h"
    
    int start_mmalcam(on_buffer_cb cb);
}
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
void on_mmalcam_buffer(MMAL_BUFFER_HEADER_T*);

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

using json = nlohmann::json;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

/// all connected clients
unordered_map<string, shared_ptr<Client>> clients{};

/// Creates peer connection and client representation
/// @param config Configuration
/// @param wws Websocket for signaling
/// @param id Client ID
/// @returns Client
shared_ptr<Client> createPeerConnection(const Configuration &config,
                                        weak_ptr<WebSocket> wws,
                                        string id);

/// Add client to stream
/// @param client Client
/// @param adding_video True if adding video
void addToStream(shared_ptr<Client> client, bool isAddingVideo);

/// Main dispatch queue
DispatchQueue MainThread("Main");

const string defaultIPAddress = "0.0.0.0";
const uint16_t defaultPort = 8000;
string ip_address = defaultIPAddress;
uint16_t port = defaultPort;

void sendInitialNalus(shared_ptr<ClientTrackData> video, uint32_t timestamp);
uint32_t last_frame_timestamp = 0;
uint32_t last_frame_duration = 0;

std::byte* s_buf = static_cast<std::byte*>(malloc(65554));
size_t s_buf_length = 0;
size_t s_data_length = 0;

std::optional<std::vector<std::byte>> previousUnitType5 = std::nullopt;
std::optional<std::vector<std::byte>> previousUnitType7 = std::nullopt;
std::optional<std::vector<std::byte>> previousUnitType8 = std::nullopt;
std::byte* start_ptr;

bool pending_frame = false;

std::string localId;

int run_websocket_server();

class GPIO {
    private:
    int _pin = 0;
    public:
    GPIO(int pin) {
        _pin = pin;
        gpioSetMode(pin, PI_OUTPUT);
    }

    ~GPIO() {}

    int servo(int pwm) {
        return gpioServo(_pin, pwm);
    }
    
};

GPIO *bldc, *steer;

int main(int argc, char **argv) try {
    bool enableDebugLogs = false;
    bool printHelp = false;
    int c = 0;
    auto parser = ArgParser({{"a", "audio"}, {"b", "video"}, {"d", "ip"}, {"p","port"}}, {{"h", "help"}, {"v", "verbose"}});
    auto parsingResult = parser.parse(argc, argv, [](string key, string value) {
        if (key == "ip") {
            ip_address = value;
        } else if (key == "port") {
            port = atoi(value.data());
        } else {
            cerr << "Invalid option --" << key << " with value " << value << endl;
            return false;
        }
        return true;
    }, [&enableDebugLogs, &printHelp](string flag){
        if (flag == "verbose") {
            enableDebugLogs = true;
        } else if (flag == "help") {
            printHelp = true;
        } else {
            cerr << "Invalid flag --" << flag << endl;
            return false;
        }
        return true;
    });
    if (!parsingResult) {
        return 1;
    }

    if (printHelp) {
        cout << "usage: stream-h264 [-a opus_samples_folder] [-b h264_samples_folder] [-d ip_address] [-p port] [-v] [-h]" << endl
        << "Arguments:" << endl
        << "\t -d " << "Signaling server IP address (default: " << defaultIPAddress << ")." << endl
        << "\t -p " << "Signaling server port (default: " << defaultPort << ")." << endl
        << "\t -v " << "Enable debug logs." << endl
        << "\t -h " << "Print this help and exit." << endl;
        return 0;
    }
    if (enableDebugLogs) {
        InitLogger(LogLevel::Debug);
    }

    std::thread websocket_thread(run_websocket_server);
    if (gpioInitialise() >= 0) {
        std::cout << "GPIO working" << std::endl;
        bldc = new GPIO(12);
        bldc->servo(1500);

        steer = new GPIO(13);
        steer->servo(1500);
    } 

    std::thread mmalcam_thread(start_mmalcam, &on_mmalcam_buffer);
    int pwm = 1500;
    while(true) {
        cin >> pwm;
        if (pwm >= 1000 && pwm <= 2000) {
            bldc->servo(pwm);
        } else {
            std::cout << "Wrong data" << std::endl;
            pwm = 1500;
        }
        cin.ignore();
    }

    cout << "Cleaning up..." << endl;
    gpioTerminate();
    return 0;

} catch (const std::exception &e) {
    std::cout << "Error: " << e.what() << std::endl;
    return -1;
}

shared_ptr<ClientTrackData> addVideo(const shared_ptr<PeerConnection> pc, const uint8_t payloadType, const uint32_t ssrc, const string cname, const string msid, const function<void (void)> onOpen) {
    auto video = Description::Video(cname);
    video.addH264Codec(payloadType);
    video.addSSRC(ssrc, cname, msid, cname);
    auto track = pc->addTrack(video);
    // create RTP configuration
    auto rtpConfig = make_shared<RtpPacketizationConfig>(ssrc, cname, payloadType, H264RtpPacketizer::defaultClockRate);
    // create packetizer
    auto packetizer = make_shared<H264RtpPacketizer>(H264RtpPacketizer::Separator::Length, rtpConfig);
    // create H264 handler
    auto h264Handler = make_shared<H264PacketizationHandler>(packetizer);
    // add RTCP SR handler
    auto srReporter = make_shared<RtcpSrReporter>(rtpConfig);
    h264Handler->addToChain(srReporter);
    // add RTCP NACK handler
    auto nackResponder = make_shared<RtcpNackResponder>();
    h264Handler->addToChain(nackResponder);
    // set handler
    track->setMediaHandler(h264Handler);
    track->onOpen(onOpen);
    auto trackData = make_shared<ClientTrackData>(track, srReporter);
    return trackData;
}

// Create and setup a PeerConnection
shared_ptr<Client> createPeerConnection(const Configuration &config,
                                                weak_ptr<WebSocket> wws,
                                                string id) {
    auto pc = make_shared<PeerConnection>(config);
    auto client = make_shared<Client>(pc);

    pc->onStateChange([id](PeerConnection::State state) {
        cout << "State: " << state << endl;
        if (state == PeerConnection::State::Disconnected ||
            state == PeerConnection::State::Failed ||
            state == PeerConnection::State::Closed) {
            // remove disconnected client
            MainThread.dispatch([id]() {
                clients.erase(id);
            });

            bldc->servo(1500);
        }
    });

    pc->onGatheringStateChange(
        [wpc = make_weak_ptr(pc), id, wws](PeerConnection::GatheringState state) {
        cout << "Gathering State: " << state << endl;
        if (state == PeerConnection::GatheringState::Complete) {
            if(auto pc = wpc.lock()) {
                auto description = pc->localDescription();
                json message = {
                    {"id", id},
                    {"type", description->typeString()},
                    {"sdp", string(description.value())}
                };
                // Gathering complete, send answer
                if (auto ws = wws.lock()) {
                    ws->send(message.dump());
                }
            }
        }
    });

    client->video = addVideo(pc, 102, 1, "video-stream", "stream1", [id, wc = make_weak_ptr(client)]() {
        MainThread.dispatch([wc]() {
            if (auto c = wc.lock()) {
                addToStream(c, true);
            }
        });
        cout << "Video from " << id << " opened" << endl;
    });

    auto dc = pc->createDataChannel("ping-pong");
    dc->onOpen([id, wdc = make_weak_ptr(dc)]() {
        // if (auto dc = wdc.lock()) {
        //     dc->send("Ping");
        // }
    });

    dc->onMessage(nullptr, [id, wdc = make_weak_ptr(dc)](string msg) {
        /*
            {x: 100,y:100}
        */
        // cout << "Message from " << id << " received: " << msg << endl;
        // if (auto dc = wdc.lock()) {
        //     dc->send("Ping");
        // }

        // data holds either std::string or rtc::binary

        nlohmann::json message = nlohmann::json::parse(msg);

        auto it = message.find("x");
        if (it != message.end()) {
            auto x = it->get<int>();
            steer->servo(x);
        }
        it = message.find("y");
        if (it != message.end()) {
            auto y = it->get<int>();
            bldc->servo(y);
        }
    });
    client->dataChannel = dc;
    clients.emplace(id, client);
    pc->setLocalDescription();
    return client;
};


/// Add client to stream
/// @param client Client
/// @param adding_video True if adding video
void addToStream(shared_ptr<Client> client, bool isAddingVideo) {
    client->setState(Client::State::Ready);
    sendInitialNalus(client->video.value(), last_frame_timestamp);
}
size_t start_offset = 0;
size_t payload_start_offset = 0;
size_t payload_size = 0;
H264::NaluIndex nalu_index = {0, 0, 0};
size_t nalu_size = 0;

void on_mmalcam_buffer(MMAL_BUFFER_HEADER_T* buffer) {
    if (pending_frame) {
        pending_frame = false;
    } else {
        s_buf_length = 0;
        if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG) {
           pending_frame = true;
        }
    }

    last_frame_duration = buffer->pts - last_frame_timestamp;
    last_frame_timestamp = buffer->pts;

    // std::vector<H264::NaluIndex> nalu_indices = H264::FindNaluIndices(buffer->data, buffer->length);

    // for (auto jt = nalu_indices.begin(); jt < nalu_indices.end(); ++jt) {
    //     size_t start_offset = jt->start_offset;
    //     size_t payload_start_offset = jt->payload_start_offset;
    //     size_t payload_size = jt->payload_size;
        
    //     start_ptr = s_buf + s_buf_length;
    //     s_buf_length += 4 + payload_size;

    //     memcpy(start_ptr + 4, buffer->data + payload_start_offset, payload_size);

    //     *(start_ptr)        = static_cast<std::byte>((payload_size >> 24) & 0xFF);
    //     *(start_ptr + 1)    = static_cast<std::byte>((payload_size >> 16) & 0xFF);
    //     *(start_ptr + 2)    = static_cast<std::byte>((payload_size >> 8) & 0xFF);
    //     *(start_ptr + 3)    = static_cast<std::byte>((payload_size >> 0) & 0xFF);

    //     auto type = H264::ParseNaluType(*(reinterpret_cast<std::uint8_t*>(start_ptr + 4)));;
    //     switch (type) {
    //         case 7:
    //             previousUnitType7 = {start_ptr + 4, start_ptr + s_buf_length};
    //             break;
    //         case 8:
    //             previousUnitType8 = {start_ptr + 4, start_ptr + s_buf_length};
    //             break;
    //         case 5:
    //             previousUnitType5 = {start_ptr + 4, start_ptr + s_buf_length};
    //             break;
    //     }
    // }

    /*...............................................................*/
    uint32_t buffer_size = buffer->length;
    uint8_t* buffer_data = buffer->data;
    nalu_size = 0;

    // struct NaluIndex {
    // // Start index of NALU, including start sequence.
    // size_t start_offset;
    // // Start index of NALU payload, typically type header.
    // size_t payload_start_offset;
    // // Length of NALU payload, in bytes, counting from payload_start_offset.
    // size_t payload_size;
    // };

    if (buffer_size < H264::kNaluShortStartSequenceSize)
    {
        // return nalu_indices;
    }

    static_assert(H264::kNaluShortStartSequenceSize >= 2,
                  "H264::kNaluShortStartSequenceSize must be larger or equals to 2");
    const size_t end = buffer_size - H264::kNaluShortStartSequenceSize;
    for (size_t i = 0; i < end;)
    {
        if (buffer_data[i + 2] > 1)
        {
                i += 3;
        }
        else if (buffer_data[i + 2] == 1)
        {
                if (buffer_data[i + 1] == 0 && buffer_data[i] == 0)
                {
                    // We found a start sequence, now check if it was a 3 of 4 byte one.
                    // NaluIndex index = {i, i + 3, 0};
                    start_offset = i;
                    payload_start_offset = i + 3;
                    payload_size = 0;
                    if (start_offset > 0 && buffer_data[start_offset - 1] == 0)
                        --start_offset;

                    // // Update length of previous entry.
                    // auto it = nalu_indices.rbegin();
                    // if (it != nalu_indices.rend())
                    //     it->payload_size = nalu_index.start_offset - it->payload_start_offset;
                    if (nalu_size > 0) {
                        payload_size = start_offset - nalu_index.payload_start_offset;
                        // nalu_index.payload_size = payload_size;
                    }

                    /****************************************/
                    // size_t start_offset = jt->start_offset;
                    // size_t payload_start_offset = jt->payload_start_offset;
                    // size_t payload_size = jt->payload_size;
                    
                    start_ptr = s_buf + s_buf_length;
                    s_buf_length += 4 + payload_size;

                    memcpy(start_ptr + 4, buffer_data + nalu_index.payload_start_offset, payload_size);

                    *(start_ptr)        = static_cast<std::byte>((payload_size >> 24) & 0xFF);
                    *(start_ptr + 1)    = static_cast<std::byte>((payload_size >> 16) & 0xFF);
                    *(start_ptr + 2)    = static_cast<std::byte>((payload_size >> 8) & 0xFF);
                    *(start_ptr + 3)    = static_cast<std::byte>((payload_size >> 0) & 0xFF);

                    auto type = H264::ParseNaluType(*(reinterpret_cast<std::uint8_t*>(start_ptr + 4)));;
                    switch (type) {
                        case 7:
                            previousUnitType7 = {start_ptr + 4, start_ptr + s_buf_length};
                            break;
                        case 8:
                            previousUnitType8 = {start_ptr + 4, start_ptr + s_buf_length};
                            break;
                        case 5:
                            previousUnitType5 = {start_ptr + 4, start_ptr + s_buf_length};
                            break;
                    }
                    /***************************************/
                    nalu_index.start_offset = start_offset;
                    nalu_index.payload_start_offset = payload_start_offset;

                    nalu_size++;

                    // nalu_indices.push_back(nalu_index);
                }

                i += 3;
        }
        else
        {
                ++i;
        }
    }

    // Update length of last entry, if any.
    // auto it = nalu_indices.rbegin();
    if (nalu_size > 0) {
        /****************************************/
        // size_t start_offset = jt->start_offset;
        // size_t payload_start_offset = jt->payload_start_offset;
        // size_t payload_size = jt->payload_size;
        payload_size = start_offset - nalu_index.payload_start_offset;

        start_ptr = s_buf + s_buf_length;
        s_buf_length += 4 + payload_size;

        memcpy(start_ptr + 4, buffer_data + nalu_index.payload_start_offset, payload_size);

        *(start_ptr) = static_cast<std::byte>((payload_size >> 24) & 0xFF);
        *(start_ptr + 1) = static_cast<std::byte>((payload_size >> 16) & 0xFF);
        *(start_ptr + 2) = static_cast<std::byte>((payload_size >> 8) & 0xFF);
        *(start_ptr + 3) = static_cast<std::byte>((payload_size >> 0) & 0xFF);

        auto type = H264::ParseNaluType(*(reinterpret_cast<std::uint8_t *>(start_ptr + 4)));
        ;
        switch (type)
        {
        case 7:
                previousUnitType7 = {start_ptr + 4, start_ptr + s_buf_length};
                break;
        case 8:
                previousUnitType8 = {start_ptr + 4, start_ptr + s_buf_length};
                break;
        case 5:
                previousUnitType5 = {start_ptr + 4, start_ptr + s_buf_length};
                break;
        }
        /***************************************/
    }

    /*...............................................................*/
    if (!pending_frame) {
        /** Last working copy**/
        for(auto id_client: clients) {
            auto id = id_client.first;
            auto client = id_client.second;
            auto optTrackData = client->video;
            if (client->getState() == Client::State::Ready && optTrackData.has_value()) {
                auto trackData = optTrackData.value();
                auto rtpConfig = trackData->sender->rtpConfig;

                // // sample time is in us, we need to convert it to seconds
                auto elapsedSeconds = double(last_frame_duration) / (1000 * 1000);
                // get elapsed time in clock rate
                uint32_t elapsedTimestamp = rtpConfig->secondsToTimestamp(elapsedSeconds);
                // set new timestamp
                rtpConfig->timestamp = rtpConfig->startTimestamp + elapsedTimestamp;

                // get elapsed time in clock rate from last RTCP sender report
                auto reportElapsedTimestamp = rtpConfig->timestamp - trackData->sender->lastReportedTimestamp();
                // check if last report was at least 1 second ago
                if (rtpConfig->timestampToSeconds(reportElapsedTimestamp) > 1) {
                    trackData->sender->setNeedsToReport();
                }

                trackData->track->send(s_buf, s_buf_length);
            }
        }
    }
}

vector<byte> initialNALUS() {
    vector<byte> units{};
    if (previousUnitType7.has_value()) {
        auto nalu = previousUnitType7.value();
        units.insert(units.end(), nalu.begin(), nalu.end());
    }
    if (previousUnitType8.has_value()) {
        auto nalu = previousUnitType8.value();
        units.insert(units.end(), nalu.begin(), nalu.end());
    }
    if (previousUnitType5.has_value()) {
        auto nalu = previousUnitType5.value();
        units.insert(units.end(), nalu.begin(), nalu.end());
    }
    return units;
}

/// Send previous key frame so browser can show something to user
/// @param stream Stream
/// @param video Video track data
void sendInitialNalus(shared_ptr<ClientTrackData> video, uint32_t timestamp) {
    auto initialNalus = initialNALUS();

    // send previous NALU key frame so users don't have to wait to see stream works
    if (!initialNalus.empty()) {
        const double frameDuration_s = double(last_frame_duration) / (1000 * 1000);
        const uint32_t frameTimestampDuration = video->sender->rtpConfig->secondsToTimestamp(frameDuration_s);
        video->sender->rtpConfig->timestamp = video->sender->rtpConfig->startTimestamp - frameTimestampDuration * 2;
        video->track->send(initialNalus);
        video->sender->rtpConfig->timestamp += frameTimestampDuration;
        video->track->send(initialNalus);
    }
}

// Helper function to generate a random ID
std::string randomId(size_t length) {
	using std::chrono::high_resolution_clock;
	static thread_local std::mt19937 rng(
	    static_cast<unsigned int>(high_resolution_clock::now().time_since_epoch().count()));
	static const std::string characters(
	    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
	std::string id(length, '0');
	std::uniform_int_distribution<int> uniform(0, int(characters.size() - 1));
	std::generate(id.begin(), id.end(), [&]() { return characters.at(uniform(rng)); });
	return id;
}

int run_websocket_server() {
	rtc::Configuration config;
    config.disableAutoNegotiation = true;

	localId = randomId(4);
	std::cout << "The local ID is " << localId << std::endl;

    rtc::WebSocketServer::Configuration serverConfig;
    serverConfig.port = port;
    serverConfig.enableTls = false;

    rtc::WebSocketServer server(std::move(serverConfig));

    std::shared_ptr<rtc::WebSocket> client;
    server.onClient([&config, &client](std::shared_ptr<rtc::WebSocket> incoming) {
		std::cout << "WebSocketServer: Client connection received" << std::endl;
		client = incoming;

		if(auto addr = client->remoteAddress())
			std::cout << "WebSocketServer: Client remote address is " << *addr << std::endl;

		client->onOpen([wclient = make_weak_ptr(client)]() {
			std::cout << "WebSocketServer: Client connection open" << std::endl;
			if(auto client = wclient.lock())
				if(auto path = client->path())
					std::cout << "WebSocketServer: Requested path is " << *path << std::endl;
		});

		client->onClosed([]() {
			std::cout << "WebSocketServer: Client connection closed" << std::endl;
		});

		client->onMessage([&config, wclient = make_weak_ptr(client)](std::variant<rtc::binary, std::string> data) {
            if (auto client = wclient.lock()) {
                // data holds either std::string or rtc::binary
                if (!std::holds_alternative<std::string>(data))
                    return;

                nlohmann::json message = nlohmann::json::parse(std::get<std::string>(data));

                auto it = message.find("id");
                if (it == message.end())
                    return;

                auto id = it->get<std::string>();

                it = message.find("type");
                if (it == message.end())
                    return;

                auto type = it->get<std::string>();

                std::shared_ptr<rtc::PeerConnection> pc;
                if (auto jt = clients.find(id); jt != clients.end()) {
                    pc = jt->second->peerConnection;
                    std::cout << "Found PC in clients" << std::endl;
                } else if (type == "offer") {
                    std::cout << "Answering to " + id << std::endl;
                    pc = (createPeerConnection(config, wclient, id))->peerConnection;
                } else if (type == "request") {
                    std::cout << "Offer to " + id << std::endl;
                    pc = (createPeerConnection(config, wclient, id))->peerConnection;
                }

                if (type == "offer" || type == "answer") {
                    auto sdp = message["sdp"].get<std::string>();
                    pc->setRemoteDescription(rtc::Description(sdp, type));
                    std::cout << type << " from " << id << " \n" << sdp << std::endl;
                } else if (type == "candidate") {
                    auto sdp = message["candidate"].get<std::string>();
                    auto mid = message["mid"].get<std::string>();
                    std::cout << "Candiate start" << std::endl;

                    pc->addRemoteCandidate(rtc::Candidate(sdp, mid));
                    std::cout << "Candiate complete" << std::endl;
                    
                }
            }
		});
    });
    while (true) {
	    std::this_thread::sleep_for(1s);
    }

	std::cout << "Success" << std::endl;
}