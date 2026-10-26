// First setup page: replaces the factory login. The API accepts nothing but
// these two values while the factory password is still active.
var m = document.getElementById('m');

function say(t) { m.textContent = t; }

function setpw() {
  var u = document.getElementById('u').value.trim();
  var p = document.getElementById('p').value;
  var q = document.getElementById('p2').value;
  if (!u) { say('the user must not be empty'); return; }
  if (!p) { say('the password must not be empty'); return; }
  if (p == 'admin') { say('choose a password other than the factory one'); return; }
  if (p != q) { say('the two passwords differ'); return; }

  var d = new URLSearchParams();
  d.set('web_user', u);
  d.set('web_password', p);
  fetch('/api/config', { method: 'POST', body: d.toString() })
    .then(function(r) {
      return r.text().then(function(t) {
        say(t);
        if (r.ok) setTimeout(function() { location.reload(); }, 1200);
      });
    })
    .catch(function() { say('could not save'); });
}

document.getElementById('c').addEventListener('submit', function(e) {
  e.preventDefault();
  setpw();
});
