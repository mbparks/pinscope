/*
 * field-notes.js
 *
 * Field Notes plugin for PinScope. A persistent notebook attached to the
 * device card with two kinds of data:
 *
 *   1. A free-form notes textarea (string) for context like "tested with
 *      10k pull-up, 3.3V supply, room temp."
 *   2. A list of timestamped "moments", each capturing the current state
 *      with an optional label.
 *
 * Both round-trip through PinScope's session export / import via
 * api.persist() and api.recall(). Verifies the plugin persistence API
 * works for both simple and complex value types.
 *
 * GPL-3.0-or-later
 */

PinScope.register({
  id: 'field-notes',
  name: 'Field Notes',
  version: '1.0',

  async onInit(api) {
    this.notes = (await api.recall('notes')) || '';
    this.moments = (await api.recall('moments')) || [];
    this.latestState = null;
    this._render(api);
  },

  onState(state, api) {
    // Keep the latest state so LOG MOMENT can snapshot it. Don't re-render
    // on every state packet, that would clobber the textarea selection.
    this.latestState = state;
  },

  _render(api) {
    const escapeHTML = (s) =>
      String(s)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');

    const momentRows = this.moments.length === 0
      ? '<div style="color:#5a5550;font-style:italic;font-size:10px;padding:4px 0">no moments logged yet</div>'
      : this.moments.map((m, idx) => {
          const time = new Date(m.t).toLocaleTimeString();
          const a0 = m.snapshot.a?.[0] ?? '?';
          const a1 = m.snapshot.a?.[1] ?? '?';
          return (
            '<div style="display:flex;gap:6px;padding:3px 0;border-bottom:1px solid #1a1816">' +
              '<span style="color:#8a6914;font-size:10px;min-width:70px">' + time + '</span>' +
              '<span style="flex:1;color:#d4a017;font-size:10.5px">' + escapeHTML(m.label || '(no label)') + '</span>' +
              '<span style="color:#a8a08c;font-size:10px">A0=' + a0 + ' A1=' + a1 + '</span>' +
              '<button data-del="' + idx + '" style="padding:0 6px;font-size:10px">×</button>' +
            '</div>'
          );
        }).join('');

    api.render(
      '<h3 style="margin:0 0 6px;font-size:11px;letter-spacing:.2em">FIELD NOTES</h3>' +
      '<textarea id="fn-notes" rows="3" style="width:100%;background:#161513;border:1px solid #2a2826;color:#d4a017;font-family:inherit;font-size:11px;padding:5px 7px;border-radius:2px;resize:vertical" placeholder="free-form notes about this test, setup, or measurement...">' + escapeHTML(this.notes) + '</textarea>' +
      '<div style="display:flex;gap:6px;margin-top:6px;align-items:center">' +
        '<input id="fn-label" type="text" placeholder="moment label (optional)" style="flex:1;background:#161513;border:1px solid #2a2826;color:#d4a017;font-family:inherit;font-size:11px;padding:4px 7px;border-radius:2px">' +
        '<button id="fn-log">LOG MOMENT</button>' +
      '</div>' +
      '<div style="margin-top:8px;padding-top:6px;border-top:1px solid #2a2826">' +
        '<div style="font-size:9px;letter-spacing:.18em;color:#8a6914;text-transform:uppercase;margin-bottom:4px">MOMENTS · ' + this.moments.length + '</div>' +
        momentRows +
      '</div>' +
      (this.moments.length > 0
        ? '<div style="text-align:right;margin-top:6px"><button id="fn-clear" style="font-size:10px;color:#c9554d;border-color:#c9554d">CLEAR ALL</button></div>'
        : '')
    );

    // Wire interactions. Re-attach every render; cheap and avoids stale handlers.
    const self = this;

    const notesEl = document.querySelector('#fn-notes');
    if (notesEl) {
      notesEl.oninput = () => {
        self.notes = notesEl.value;
        api.persist('notes', self.notes);
      };
    }

    const logBtn = document.querySelector('#fn-log');
    if (logBtn) {
      logBtn.onclick = () => {
        if (!self.latestState) {
          api.log('no state yet; connect a device first');
          return;
        }
        const labelEl = document.querySelector('#fn-label');
        const label = labelEl ? labelEl.value.trim() : '';
        const moment = {
          t: Date.now(),
          label,
          snapshot: {
            a: self.latestState.a ? self.latestState.a.slice() : null,
            d: self.latestState.d ? self.latestState.d.slice() : null,
            v: self.latestState.v ? self.latestState.v.slice() : null,
          },
        };
        self.moments.push(moment);
        // Keep the list bounded; oldest entries fall off after 50.
        if (self.moments.length > 50) self.moments = self.moments.slice(-50);
        api.persist('moments', self.moments);
        if (labelEl) labelEl.value = '';
        self._render(api);
      };
    }

    document.querySelectorAll('button[data-del]').forEach((btn) => {
      btn.onclick = () => {
        const idx = parseInt(btn.dataset.del, 10);
        self.moments.splice(idx, 1);
        api.persist('moments', self.moments);
        self._render(api);
      };
    });

    const clearBtn = document.querySelector('#fn-clear');
    if (clearBtn) {
      clearBtn.onclick = () => {
        if (self.moments.length === 0) return;
        if (clearBtn.dataset.confirm === 'yes') {
          self.moments = [];
          api.persist('moments', self.moments);
          self._render(api);
        } else {
          clearBtn.dataset.confirm = 'yes';
          clearBtn.textContent = 'CONFIRM?';
          setTimeout(() => {
            if (clearBtn.dataset.confirm === 'yes') {
              clearBtn.dataset.confirm = '';
              clearBtn.textContent = 'CLEAR ALL';
            }
          }, 3000);
        }
      };
    }
  },
});
