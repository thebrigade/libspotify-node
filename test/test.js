var assert = require('assert');
var util = require('util');

var account = require('../account');
var spotify = require('../spotify');

// make these modules available to any module which require()s this module
GLOBAL.sys = util;
GLOBAL.assert = assert;
GLOBAL.spotify = spotify;

GLOBAL.createSession = function(dontForwardLogging, onsession) {
  if (typeof dontForwardLogging === 'function') {
    onsession = dontForwardLogging;
    dontForwardLogging = false;
  }

  var session = new spotify.Session({applicationKey: account.applicationKey});

  if (!dontForwardLogging) {
      process.stderr.write(JSON.stringify(session));
      session.on('logMessage', function (message) {
      util.log(message.substr(0, message.length - 1));
    });
  }

  session.login(account.username, account.password, function (err) {
    assert.ifError(err);
    onsession(session);
  });
}

process.on('uncaughtException', function (err) {
  console.error(err.stack || err);
  process.exit(2);
});
