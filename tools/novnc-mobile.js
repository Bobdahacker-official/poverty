import UI from './app/ui.js';

const pacedConnections = new WeakSet();
function paceKeyboard() {
    const rfb = UI.rfb;
    if (!UI.connected || !rfb || pacedConnections.has(rfb)) return;
    pacedConnections.add(rfb);
    const send = rfb.sendKey.bind(rfb);
    let queue = [];
    let timer;
    function next() {
        if (!UI.connected || UI.rfb !== rfb) {
            queue = [];
            timer = undefined;
            return;
        }
        const event = queue.shift();
        // Keep text and special keys on the same ordered VNC transport.
        if (event) send(event[0], undefined, event[2]);
        timer = queue.length ? setTimeout(next, 20) : undefined;
    }
    rfb.sendKey = (symbol, code, down) => {
        if (down === undefined) {
            queue.push([symbol, code, true], [symbol, code, false]);
        } else {
            queue.push([symbol, code, down]);
        }
        if (timer === undefined) timer = setTimeout(next, 0);
    };
}
new MutationObserver(paceKeyboard).observe(document.documentElement, {
    attributes: true,
    attributeFilter: ['class'],
});
paceKeyboard();

const input = document.getElementById('noVNC_keyboardinput');
input.setAttribute('inputmode', 'text');
input.setAttribute('enterkeyhint', 'enter');
input.setAttribute('autocorrect', 'off');
input.setAttribute('autocapitalize', 'off');
input.setAttribute('autocomplete', 'off');
input.spellcheck = false;

const toolbar = document.createElement('div');
toolbar.id = 'magenta-mobile-toolbar';
toolbar.setAttribute('role', 'toolbar');
toolbar.setAttribute('aria-label', 'Remote keyboard');
const toggle = document.createElement('button');
toggle.type = 'button';
toggle.textContent = 'Keyboard';
toggle.setAttribute('aria-label', 'Show mobile keyboard');
toggle.setAttribute('aria-pressed', 'false');
toolbar.append(toggle);
const tools = document.createElement('div');
tools.id = 'magenta-mobile-keys';
tools.hidden = true;
toolbar.append(tools);
document.body.append(toolbar);

function focusKeyboard() {
    input.focus({preventScroll: true});
    input.setSelectionRange(input.value.length, input.value.length);
}

toggle.addEventListener('click', () => {
    if (document.activeElement === input) {
        input.blur();
    } else {
        UI.closeControlbar();
        focusKeyboard();
    }
});

const modifiers = new Map();
for (const [label, symbol, code, modifier] of [
    ['Enter', 0xff0d, 'Enter', false],
    ['⌫', 0xff08, 'Backspace', false],
    ['Tab', 0xff09, 'Tab', false],
    ['Esc', 0xff1b, 'Escape', false],
    ['Ctrl', 0xffe3, 'ControlLeft', true],
    ['Alt', 0xffe9, 'AltLeft', true],
    ['←', 0xff51, 'ArrowLeft', false],
    ['→', 0xff53, 'ArrowRight', false],
    ['↑', 0xff52, 'ArrowUp', false],
    ['↓', 0xff54, 'ArrowDown', false],
]) {
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = label;
    button.setAttribute('aria-label', code);
    if (modifier) {
        button.setAttribute('aria-pressed', 'false');
        modifiers.set(code, {button, symbol, down: false});
    }
    button.addEventListener('pointerdown', event => event.preventDefault());
    button.addEventListener('click', () => {
        if (!UI.connected || !UI.rfb) return;
        if (modifier) {
            const state = modifiers.get(code);
            state.down = !state.down;
            UI.rfb.sendKey(symbol, code, state.down);
            button.setAttribute('aria-pressed', String(state.down));
        } else {
            UI.rfb.sendKey(symbol, code);
        }
        focusKeyboard();
    });
    tools.append(button);
}

function releaseModifiers() {
    for (const [code, state] of modifiers) {
        if (state.down && UI.connected && UI.rfb) {
            UI.rfb.sendKey(state.symbol, code, false);
        }
        state.down = false;
        state.button.setAttribute('aria-pressed', 'false');
    }
}

input.addEventListener('focus', () => {
    toolbar.classList.add('keyboard-open');
    tools.hidden = false;
    toggle.textContent = 'Hide keyboard';
    toggle.setAttribute('aria-label', 'Hide mobile keyboard');
    toggle.setAttribute('aria-pressed', 'true');
});
input.addEventListener('blur', () => {
    toolbar.classList.remove('keyboard-open');
    releaseModifiers();
    tools.hidden = true;
    toggle.textContent = 'Keyboard';
    toggle.setAttribute('aria-label', 'Show mobile keyboard');
    toggle.setAttribute('aria-pressed', 'false');
});
window.addEventListener('blur', releaseModifiers);
document.addEventListener('visibilitychange', () => {
    if (document.hidden) releaseModifiers();
});

function positionToolbar() {
    const viewport = window.visualViewport;
    const inset = viewport ? Math.max(0, window.innerHeight - viewport.height - viewport.offsetTop) : 0;
    toolbar.style.bottom = `${inset + 8}px`;
}
if (window.visualViewport) {
    window.visualViewport.addEventListener('resize', positionToolbar);
    window.visualViewport.addEventListener('scroll', positionToolbar);
}
window.addEventListener('resize', positionToolbar);
positionToolbar();
