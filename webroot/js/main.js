document.getElementById("media_configurations").style.display = "none";

function toogleOptions() {
    let x = document.getElementById("media_configurations");
    if (x.style.display === "none") {
        x.style.display = "block";
        document.getElementById("showConfig").innerText = "Hide Video Settings";
    } else {
        x.style.display = "none";
        document.getElementById("showConfig").innerText = "Show Video Settings";
    };
}

// !!!! This code is experimental...don't blame me if your performance tanks.

const box = document.getElementById("video-text-container");
const textObjects = document.getElementsByClassName("video-text");
const totalText = textObjects.length
for (let i = 0; i < totalText; ++i) {
    let element = textObjects[i];
    element.parentNode.__default_width = element.parentNode.clientWidth;
    element.__default_font_size = parseInt(window.getComputedStyle(element).fontSize) * Math.min(element.parentNode.__default_width / element.scrollWidth, 1.0);
}

window.addEventListener("resize", resizeBox);

function resizeBox() {
	calcTextSize();
}
resizeBox();

function calcTextSize() {
    for (let i = 0; i < totalText; ++i) {
        let element = textObjects[i];
        const parentContainerWidth = element.parentNode.clientWidth;
        const currentTextWidth = element.scrollWidth;
        const newValue = Math.floor((parentContainerWidth / element.parentNode.__default_width) * element.__default_font_size);
    
        element.style.fontSize = newValue + 'px';
    };
}
let _connectButton = document.getElementById('Connect');
let _disconnectButton = document.getElementById('Disconnect');
_connectButton.onclick = () => {
    console.log('Starting streamer session');

    start();
};

_disconnectButton.onclick = () => {
    stop();
};