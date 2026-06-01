/*
 * servo-sweep.js
 *
 * Drive a PWM pin in a slow triangle wave (servo sweep). Demonstrates
 * api.send to issue wire-protocol commands from inside a plugin.
 * Includes start / stop controls.
 *
 * GPL-3.0-or-later
 */

PinScope.register({
  id: 'servo-sweep',
  name: 'Servo Sweep',
  version: '1.0',

  onInit(api) {
    this.running = false;
    this.duty = 0;
    this.direction = 1;
    this.pin = 9;
    this._render(api);
  },

  onState(state, api) {
    if (!this.running) return;
    this.duty += this.direction * 8;
    if (this.duty >= 255) { this.duty = 255; this.direction = -1; }
    if (this.duty <= 0)   { this.duty = 0;   this.direction =  1; }
    api.send({ cmd: 'pwm', pin: this.pin, val: this.duty });
    this._render(api);
  },

  _render(api) {
    const status = this.running ? 'SWEEPING' : 'STOPPED';
    const statusColor = this.running ? '#6ba368' : '#8a6914';
    api.render(
      '<h3>SERVO SWEEP</h3>' +
      '<p>' +
        '<label>pin <select id="pin-sel">' +
          [3, 5, 6, 9, 10, 11].map((p) =>
            '<option value="' + p + '"' + (p === this.pin ? ' selected' : '') + '>D' + p + '</option>'
          ).join('') +
        '</select></label>' +
      '</p>' +
      '<p style="color:' + statusColor + ';font-size:13px">' + status + ' · duty ' + this.duty + '/255</p>' +
      '<div style="background:#2a2826;height:6px;margin:4px 0;border-radius:1px">' +
        '<div style="background:#d4a017;height:6px;width:' + (this.duty / 255 * 100).toFixed(0) + '%"></div>' +
      '</div>' +
      '<button id="btn-start" ' + (this.running ? 'disabled' : '') + '>START</button> ' +
      '<button id="btn-stop"  ' + (this.running ? '' : 'disabled') + '>STOP</button>'
    );
    const me = this;
    const pinSel = document.querySelector('#pin-sel');
    if (pinSel) pinSel.onchange = () => {
      me.pin = parseInt(pinSel.value, 10);
      // Drop the pin into PWM mode the first time we touch it
      api.send({ cmd: 'mode', pin: me.pin, mode: 'pwm' });
    };
    const startBtn = document.querySelector('#btn-start');
    if (startBtn) startBtn.onclick = () => {
      me.running = true;
      api.send({ cmd: 'mode', pin: me.pin, mode: 'pwm' });
      me._render(api);
    };
    const stopBtn = document.querySelector('#btn-stop');
    if (stopBtn) stopBtn.onclick = () => {
      me.running = false;
      me._render(api);
    };
  },
});
