/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

var win = window.open();
win.location = "%s";
win.addEventListener("load", function(e) {
    marionetteScriptFinished(true);
  }, false);
