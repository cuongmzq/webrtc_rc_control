// const dataChannelLog = document.getElementById('data-channel');

const clientId = randomId(10);
const websocket = new WebSocket('ws://raspberrypi.local:8000/' + clientId);

websocket.onopen = () => {
    // document.getElementById('Connect').disabled = false;
    setTimeout(() => {
        start();
    }, 500);
}

websocket.onmessage = async (evt) => {
    if (typeof evt.data !== 'string') {
        return;
    }
    const message = JSON.parse(evt.data);
    if (message.type == "offer") {
        await handleOffer(message)
    }
}

let pc = null;
let dc = null;

function createPeerConnection() {
    const config = {
        bundlePolicy: "max-bundle",
    };

    let pc = new RTCPeerConnection(config);

    // Receive audio/video track
    pc.ontrack = (evt) => {
        const video = document.getElementById('video');
        // if (!video.srcObject) {
          video.srcObject = evt.streams[0]; // The stream groups audio and video tracks
        //   video.play();
            // document.getElementById('video-text-container').style.display = 'none';
        // }
    };

    // Receive data channel
    pc.ondatachannel = (evt) => {
        dc = evt.channel;

        dc.onopen = () => {
            // dataChannelLog.textContent += '- open\n';
            // dataChannelLog.scrollTop = dataChannelLog.scrollHeight;
        };

        dc.onmessage = (evt) => {
            if (typeof evt.data !== 'string') {
                return;
            }

            // dataChannelLog.textContent += '< ' + evt.data + '\n';
            // dataChannelLog.scrollTop = dataChannelLog.scrollHeight;
        }

        dc.onclose = () => {
            // dataChannelLog.textContent += '- close\n';
            // dataChannelLog.scrollTop = dataChannelLog.scrollHeight;
        };
    }

    return pc;
}

async function waitGatheringComplete() {
    return new Promise((resolve) => {
        if (pc.iceGatheringState === 'complete') {
            resolve();
        } else {
            pc.addEventListener('icegatheringstatechange', () => {
                if (pc.iceGatheringState === 'complete') {
                    resolve();
                }
            });
        }
    });
}

async function sendAnswer(pc) {
    await pc.setLocalDescription(await pc.createAnswer());
    await waitGatheringComplete();

    const answer = pc.localDescription;
    // document.getElementById('answer-sdp').textContent = answer.sdp;

    websocket.send(JSON.stringify({
        id: "server",
        type: answer.type,
        sdp: answer.sdp,
    }));
}

async function handleOffer(offer) {
    pc = createPeerConnection();
    await pc.setRemoteDescription(offer);
    await sendAnswer(pc);
}

function sendRequest() {
    websocket.send(JSON.stringify({
        id: "server",
        type: "request",
    }));
}

function start() {
    sendRequest();
}

function stop() {
    const video = document.getElementById('video');
    // video.srcObject = null;
    // close data channel
    if (dc) {
        dc.close();
        dc = null;
    }

    // close transceivers
    if (pc.getTransceivers) {
        pc.getTransceivers().forEach((transceiver) => {
            if (transceiver.stop) {
                transceiver.stop();
            }
        });
    }

    // close local audio/video
    pc.getSenders().forEach((sender) => {
        const track = sender.track;
        if (track !== null) {
            sender.track.stop();
        }
    });
    // document.getElementById('video-text-container').style.display = 'none';
    // close peer connection
    setTimeout(() => {
        pc.close();
        pc = null;
    }, 500);
}

// Helper function to generate a random ID
function randomId(length) {
  const characters = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
  const pickRandom = () => characters.charAt(Math.floor(Math.random() * characters.length));
  return [...Array(length) ].map(pickRandom).join('');
}