var fs = require('fs');
var http = require('http');
var url = require('url');
var account = require('../account');
var spotify = require('../spotify');

//var BufferedWriter = require ("buffered-writer");

var session = new spotify.Session({applicationKey: account.applicationKey});

session.login(account.username, account.password, function (err) {
    console.log("logged in...");
});


http.createServer(function(req, res){
  
  var request = url.parse(req.url, true);
  var album_id = request.pathname.slice(1);

  console.log("Request for: " + album_id);

  if (album_id.indexOf('spotify:album:') == 0) {

	// Todo: Check for file in (our) cache first

    session.getAlbumImageByLink(album_id, function(err, size, bytes) {
		
			if(err) {
				console.log(err);
				res.writeHead(200, {'Content-Type': 'text/plain' });
			     res.end('Unkown Album');
				return;
			} 
			
		console.log('got album image:' + size + ' bytes');
		
		// Save to disk	
		/*	new BufferedWriter (album_id)
			    .on ("error", function (error){
			        console.log (error);
			    }).write (bytes, 0, size) .close ();
				
				res.writeHead(200, {'Content-Type': 'image/jpeg' });
			    res.end(new Buffer(bytes), 'binary');
		*/
	  });
     
  } else { 
     res.writeHead(200, {'Content-Type': 'text/plain' });
     res.end('Unkown Album');
  }
}).listen(8080, '127.0.0.1');