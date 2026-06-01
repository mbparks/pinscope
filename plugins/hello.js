/*
 * hello.js
 *
 * Minimal PinScope plugin. Demonstrates the register call, onInit,
 * onState, and api.render. Just prints A0 to a panel.
 *
 * GPL-3.0-or-later
 */

PinScope.register({
  id: 'hello',
  name: 'Hello World',
  version: '1.0',

  onInit(api) {
    api.render('<p>waiting for first state packet...</p>');
  },

  onState(state, api) {
    const a0 = state.a[0];
    const bars = '█'.repeat(Math.floor(a0 / 100));
    api.render(
      '<h3>A0 LIVE</h3>' +
      '<p style="font-size:14px">' + a0 + ' / 1023</p>' +
      '<p style="color:#f5b724">' + bars + '</p>'
    );
  },
});
