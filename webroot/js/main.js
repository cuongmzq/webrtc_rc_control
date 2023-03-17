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

let data = {
    x: 1500,
    y: 1500
};

let dataX = 1500;
let dataY = 1500;
let previousTime;
let timerSendDC = 0;
let sendDCInterval = 20; //milliseconds

function loop(timeStamp = 0) {
    let dt;
    if (previousTime == undefined) {
        dt = 0;
    } else {
        dt = timeStamp - previousTime;
    }
    updateSteeringValue(dt);
    updateThrottleValue(dt);


    previousTime = timeStamp;

    timerSendDC += dt;

    if (timerSendDC >= sendDCInterval) {
        timerSendDC = 0;
        if (dc && (dc.readyState == "open")) {
            data.x = Math.round(dataX);
            data.y = Math.round(dataY);
            dc.send(JSON.stringify(data));
        }
    }

    requestAnimationFrame(loop);
}

requestAnimationFrame(loop);

var scrollIndicator = document.getElementById('indicator');
var scrollIndicator2 = document.getElementById('indicator2');

let targetSteeringValue = 1500;
let targetThrottleValue = 1500;

let moveFactor = 0.05;
let moveFactor2 = 0.05;

let THROTTLE_MIN = 1200;
let THROTTLE_MID = 1500;
let THROTTLE_MAX = 1600;

const STEER_MIN = 1350;
const STEER_MID = 1560;
const STEER_MAX = 1700;

function updateSteeringValue(dt) {
    dataX = dataX + (targetSteeringValue - dataX) * moveFactor * dt * 60 / 1000;

    var transformString = 'translateX('+(((dataX - STEER_MIN) / (STEER_MAX - STEER_MIN))*300)+'px)';
    scrollIndicator.style.mozTransform = transformString;
    scrollIndicator.style.webkitTransform = transformString;
    scrollIndicator.style.transform = transformString;
}

function updateThrottleValue(dt) {
    dataY = dataY + (targetThrottleValue - dataY) * moveFactor2 * dt * 60 / 1000;

    var transformString = 'translateX('+(((dataY - THROTTLE_MIN) / (THROTTLE_MAX - THROTTLE_MIN))*300)+'px)';
    scrollIndicator2.style.mozTransform = transformString;
    scrollIndicator2.style.webkitTransform = transformString;
    scrollIndicator2.style.transform = transformString;
}

// Add event listener on keydown
document.addEventListener('keydown', (event) => {
    // event.preventDefault();
    var name = event.key;
    var code = event.code;
    // Alert the key name and key code on keydown
    // console.log(`Key pressed ${name} \r\n Key code value: ${code}`);
    // if (dc) {
        switch(code) {
            case "KeyA":
                targetSteeringValue = STEER_MIN;
                moveFactor = 0.02;
                break;
            case "KeyD":
                targetSteeringValue = STEER_MAX;
                moveFactor = 0.02;
                break;
            case "KeyW":
                targetThrottleValue = THROTTLE_MIN;
                moveFactor2 = 0.005;
                break;    
            case "KeyS":
                targetThrottleValue = THROTTLE_MAX;
                moveFactor2 = 0.005;
                break;
        }
    // }
    
  }, false);

  // Add event listener on keydown
document.addEventListener('keyup', (event) => {
    // event.preventDefault();
    var name = event.key;
    var code = event.code;
    // Alert the key name and key code on keydown
    // console.log(`Key pressed ${name} \r\n Key code value: ${code}`);
    // if (dc) {
        switch(code) {
            case "KeyA":
                targetSteeringValue = STEER_MID;
                moveFactor = 0.05;

                break;
            case "KeyD":
                targetSteeringValue = STEER_MID;
                moveFactor = 0.05;

                break;
            case "KeyW":
                targetThrottleValue = THROTTLE_MID;
                dataY = 1500;
                break;
            case "KeyS":
                targetThrottleValue = THROTTLE_MID;
                dataY = 1500;
                break;
        }
    // }
    
  }, false);

let btnUp = document.getElementById('btn_up');
let btnDown = document.getElementById('btn_down');
let btnLeft = document.getElementById('btn_left');
let btnRight = document.getElementById('btn_right');

btnUp.addEventListener('mousedown', () => {
    targetThrottleValue = THROTTLE_MIN;
    moveFactor2 = 0.005;
});

btnUp.addEventListener('mouseup', () => {
    targetThrottleValue = THROTTLE_MID;
    dataY = 1500;
    // moveFactor2 = 0.08;
});

btnDown.addEventListener('mousedown', () => {
    targetThrottleValue = THROTTLE_MAX;
    moveFactor2 = 0.08;
});

btnDown.addEventListener('mouseup', () => {
    targetThrottleValue = THROTTLE_MID;
    dataY = 1500;
    // moveFactor2 = 0.1;
});

btnLeft.addEventListener('mousedown', () => {
    targetSteeringValue = STEER_MIN;
    moveFactor = 0.025;
});

btnLeft.addEventListener('mouseup', () => {
    targetSteeringValue = STEER_MID;
    moveFactor = 0.05;
});

btnRight.addEventListener('mousedown', () => {
    targetSteeringValue = STEER_MAX;
    moveFactor = 0.025;
});

btnRight.addEventListener('mouseup', () => {
    targetSteeringValue = STEER_MID;
    moveFactor = 0.05;
});
