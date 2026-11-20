//
// Author: Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// Script of the static-site example: shows that it ran and handles a button.
//

document.addEventListener('DOMContentLoaded', function () {
  var status = document.getElementById('status');
  status.textContent = 'JavaScript loaded';
  status.classList.add('ok');

  document.getElementById('greet').addEventListener('click', function () {
    var time = new Date().toLocaleTimeString();
    document.getElementById('greeting').textContent = 'Hello! The time is ' + time + '.';
  });
});
