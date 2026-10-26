// Settings page. GET /api/config returns the editable values as form encoded
// key=value pairs, POST writes back what the form holds. The markup carries
// the labels, units and client side limits, the firmware clamps again.
var f = document.getElementById('f');
var m = document.getElementById('m');
var at = document.getElementById('alarm_time');

function say(t) { m.textContent = t; }

// Unit bridge between the API and the field: seconds in the field,
// milliseconds in the setting, for example.
function scale(i) { return i.getAttribute('data-scale') || 1; }

function pad2(v) {
  v = parseInt(v, 10);
  if (isNaN(v)) return '00';
  return (v < 10 ? '0' : '') + v;
}

// The alarm time is one <input type=time> here and two settings
// (alarm_hour, alarm_minute) in the firmware, so it lives outside the
// generic loop below.
function loadAlarmTime(d) {
  var h = d.get('alarm_hour');
  var mi = d.get('alarm_minute');
  if (h !== null && mi !== null) at.value = pad2(h) + ':' + pad2(mi);
  var hd = d.get('alarm_hour.def');
  var md = d.get('alarm_minute.def');
  if (hd !== null && md !== null) {
    document.getElementById('alarm_time.def').textContent =
      '(default ' + pad2(hd) + ':' + pad2(md) + ')';
  }
}

function load() {
  fetch('/api/config')
    .then(function(r) { return r.text(); })
    .then(function(t) {
      var d = new URLSearchParams(t);
      loadAlarmTime(d);
      Array.prototype.forEach.call(f.elements, function(i) {
        if (!i.id) return;
        if (i.id.slice(-6) == '.clear') { i.checked = false; return; }
        var v = d.get(i.id);
        if (v !== null) {
          if (i.type == 'checkbox') { i.checked = (v == '1'); }
          else { i.value = (scale(i) > 1 ? Math.round(v / scale(i)) : v); }
        }
        var s = document.getElementById(i.id + '.state'), sv = d.get(i.id + '.set');
        if (s && sv !== null) { s.textContent = (sv == '1' ? '(stored)' : '(not set)'); }
        var h = document.getElementById(i.id + '.def'), dv = d.get(i.id + '.def');
        if (h && dv !== null) {
          h.textContent = '(default ' + (scale(i) > 1 ? Math.round(dv / scale(i)) : dv) + ')';
        }
      });
      say('loaded');
    })
    .catch(function() { say('could not load'); });
}

function save() {
  var d = new URLSearchParams();

  var p = (at.value || '').split(':');
  if (p.length == 2) {
    var ah = parseInt(p[0], 10), am = parseInt(p[1], 10);
    if (!isNaN(ah) && !isNaN(am)) {
      d.set('alarm_hour', ah);
      d.set('alarm_minute', am);
    }
  }

  Array.prototype.forEach.call(f.elements, function(i) {
    if (!i.id || i.id == 'alarm_time') return;
    if (i.id.slice(-6) == '.clear') {
      var t = document.getElementById(i.id.slice(0, -6));
      if (i.checked && t && t.value) { i.checked = false; }
      else if (i.checked) { d.set(i.id, 1); i.checked = false; }
      return;
    }
    if (i.type == 'checkbox') { d.set(i.id, i.checked ? 1 : 0); return; }
    if (i.type == 'password' && i.value === '') return;
    d.set(i.id, scale(i) > 1 ? Math.round(i.value * scale(i)) : i.value);
  });

  fetch('/api/config', { method: 'POST', body: d.toString() })
    .then(function(r) {
      return r.text().then(function(t) {
        if (t.indexOf('reboot') >= 0) {
          say('saved, the box restarts and joins your WiFi');
          return;
        }
        if (t.indexOf('relogin') >= 0) {
          say('login saved, signing in again');
          setTimeout(function() { location.reload(); }, 1500);
          return;
        }
        say(r.ok ? 'saved' : 'rejected');
        setTimeout(load, 800);
      });
    })
    .catch(function() { say('could not save'); });
}

function defaults() {
  if (!confirm('Every setting back to the firmware default? WiFi, login and API key stay.')) return;
  fetch('/api/reset', { method: 'POST' })
    .then(function(r) { say(r.ok ? 'reset to defaults' : 'rejected'); setTimeout(load, 800); })
    .catch(function() { say('could not reset'); });
}

function stopServer() {
  fetch('/api/shutdown', { method: 'POST' })
    .then(function() { say('server stops now'); })
    .catch(function() {});
  setTimeout(function() { say('server stopped'); }, 3000);
}

f.addEventListener('submit', function(e) { e.preventDefault(); save(); });
document.getElementById('reset').addEventListener('click', defaults);
document.getElementById('stop').addEventListener('click', stopServer);
load();
