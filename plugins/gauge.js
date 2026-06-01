/*
 * gauge.js
 *
 * SVG arc-gauge plugin for an analog pin. Demonstrates:
 *   - persistent settings via api.persist / api.recall
 *   - SVG rendering via api.renderSVG (avoids the script-tag stripper)
 *   - a configurable pin via a button group at the top of the panel
 *
 * GPL-3.0-or-later
 */

PinScope.register({
  id: 'gauge',
  name: 'Analog Gauge',
  version: '1.1',

  async onInit(api) {
    this.pin = (await api.recall('pin')) || 0;
    this._render(api, null);
  },

  onState(state, api) {
    this._render(api, state.a[this.pin]);
  },

  _render(api, value) {
    const pin = this.pin;
    const val = value == null ? 0 : value;
    const norm = Math.max(0, Math.min(1, val / 1023));
    // Arc from 135 deg to 405 deg (270 deg sweep)
    const startAngle = 135;
    const endAngle = 135 + 270 * norm;
    const cx = 90, cy = 80, r = 56;
    const tickPath = describeArc(cx, cy, r, 135, 405);
    const valPath = describeArc(cx, cy, r, startAngle, endAngle);
    const buttons = [0, 1, 2, 3, 4, 5].map((p) =>
      '<button data-pin="' + p + '" style="margin-right:4px;' +
      (p === pin ? 'background:#d4a017;color:#0a0a09' : '') +
      '">A' + p + '</button>'
    ).join('');
    api.renderSVG(
      '<div>' + buttons + '</div>' +
      '<svg width="180" height="120" viewBox="0 0 180 120" xmlns="http://www.w3.org/2000/svg">' +
        '<path d="' + tickPath + '" stroke="#2a2826" stroke-width="6" fill="none"/>' +
        '<path d="' + valPath + '" stroke="#d4a017" stroke-width="6" fill="none" stroke-linecap="round"/>' +
        '<text x="90" y="78" text-anchor="middle" fill="#f5b724" font-family="JetBrains Mono" font-size="20">' + val + '</text>' +
        '<text x="90" y="98" text-anchor="middle" fill="#8a6914" font-family="JetBrains Mono" font-size="10">A' + pin + ' / 1023</text>' +
      '</svg>'
    );
    // Wire button clicks (note: this re-attaches every render; small cost,
    // acceptable for a demo).
    const me = this;
    document.querySelectorAll('button[data-pin]').forEach((b) => {
      b.onclick = () => {
        me.pin = parseInt(b.dataset.pin, 10);
        api.persist('pin', me.pin);
      };
    });
  },
});

// Helper: build an SVG arc path
function describeArc(cx, cy, r, startA, endA) {
  const start = polar(cx, cy, r, endA);
  const end = polar(cx, cy, r, startA);
  const largeArc = endA - startA <= 180 ? 0 : 1;
  return 'M ' + start.x + ' ' + start.y +
         ' A ' + r + ' ' + r + ' 0 ' + largeArc + ' 0 ' + end.x + ' ' + end.y;
}
function polar(cx, cy, r, degrees) {
  const rad = (degrees - 90) * Math.PI / 180;
  return { x: cx + r * Math.cos(rad), y: cy + r * Math.sin(rad) };
}
