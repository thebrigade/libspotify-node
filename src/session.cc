#include "session.h"
#include "user.h"
#include "search.h"
#include "track.h"

#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

typedef struct log_message {
  struct log_message *next;
  const char *message;
} log_message_t;


// userdata passed with a search query
typedef struct search_data {
  Session *session;
  Persistent<Function> *callback;
} search_data_t;

typedef struct user_data {
  Session *session;
  Persistent<Function> *callback;
} user_data_t;

static Persistent<String> log_message_symbol;

// ----------------------------------------------------------------------------
// libspotify callbacks

static void SpotifyRunloopTimerProcess(EV_P_ ev_timer *w, int revents) {
  Session *s = static_cast<Session*>(w->data);
  s->ProcessEvents();
}

static void SpotifyRunloopAsyncProcess(EV_P_ ev_async *w, int revents) {
  Session *s = static_cast<Session*>(w->data);
  s->ProcessEvents();
}

static void NotifyMainThread(sp_session* session) {
  // Called by a background thread (controlled by libspotify) when we need to
  // query sp_session_process_events, which is handled by
  // Session::ProcessEvents. ev_async_send queues a call on the main ev runloop.
  Session* s = static_cast<Session*>(sp_session_userdata(session));
  ev_async_send(EV_DEFAULT_UC_ s->runloop_async_);
}

void Session::ProcessEvents() {
  int timeout = 0;
  ev_timer_stop(EV_DEFAULT_UC_ runloop_timer_);

  if (session_)
    sp_session_process_events(session_, &timeout);

  ev_timer_set(runloop_timer_, timeout / 1000.0, 0.0);
  ev_timer_start(EV_DEFAULT_UC_ runloop_timer_);
}

void Session::DequeueLogMessages() {
  while (!log_message_queue_.empty()) {
    const char* message = log_message_queue_.front();
    log_message_queue_.pop();
    Local<Value> argv[] = { String::New(message) };
    //Emit(log_message_symbol, 1, argv); // TODO masterfix
    delete message;
  }
}

static void SpotifyRunloopAsyncLogMessage(EV_P_ ev_async *w, int revents) {
  Session *s = static_cast<Session*>(w->data);
  s->DequeueLogMessages();
}

static void LogMessage(sp_session* session, const char* data) {
  Session* s = static_cast<Session*>(sp_session_userdata(session));
  if (pthread_self() == s->main_thread_id_) {
    // Called from the main runloop thread -- emit directly
    Local<Value> argv[] = { String::New(data) };
    //s->Emit(log_message_symbol, 1, argv); // TODO masterfix
  } else {
    // Called from a background thread -- queue and notify
    const char* message = strdup(data);

    if (message == NULL)
      return;

    s->log_message_queue_.push(message);

    // Signal we need to dequeue the message queue (handled by
    // SpotifyRunloopAsyncLogMessage).
    ev_async_send(EV_DEFAULT_UC_ s->logmsg_async_);
  }
}

static void MessageToUser(sp_session* session, const char* data) {
  Session* s = reinterpret_cast<Session*>(sp_session_userdata(session));
  assert(s->main_thread_id_ == pthread_self() /* or we will crash */);
  Local<Value> argv[] = { String::New(data) };
  //s->Emit(String::New("message_to_user"), 1, argv); // TODO masterfix
}

static void LoggedOut(sp_session* session) {
  Session* s = reinterpret_cast<Session*>(sp_session_userdata(session));
  assert(s->main_thread_id_ == pthread_self() /* or we will crash */);
  if (s->logout_callback_) {
    assert((*s->logout_callback_)->IsFunction());
    (*s->logout_callback_)->Call(s->handle_, 0, NULL);
    cb_destroy(s->logout_callback_);
    s->logout_callback_ = NULL;
  }
  ev_unref(EV_DEFAULT_UC);
  s->DequeueLogMessages();
}

static void LoggedIn(sp_session* session, sp_error error) {
  Session* s = reinterpret_cast<Session*>(sp_session_userdata(session));
  assert(s->login_callback_ != NULL);
  assert((*s->login_callback_)->IsFunction());
  assert(s->main_thread_id_ == pthread_self() /* or we will crash */);
  if (error != SP_ERROR_OK) {
    Local<Value> argv[] = {
      Exception::Error(String::New(sp_error_message(error))) };
    (*s->login_callback_)->Call(s->handle_, 1, argv);
  } else {
    (*s->login_callback_)->Call(s->handle_, 0, NULL);
  }
  cb_destroy(s->login_callback_);
  s->login_callback_ = NULL;
}

static void MetadataUpdated(sp_session *session) {
  Session* s = reinterpret_cast<Session*>(sp_session_userdata(session));
  assert(s->main_thread_id_ == pthread_self() /* or we will crash */);
  //s->Emit(String::New("metadataUpdated"), 0, NULL); // TODO masterfix
  s->metadata_update_queue_.process(s->session_, s->handle_);
}

static void ConnectionError(sp_session* session, sp_error error) {
  Session* s = reinterpret_cast<Session*>(sp_session_userdata(session));
  assert(s->main_thread_id_ == pthread_self() /* or we will crash */);
  Local<Value> argv[] = { String::New(sp_error_message(error)) };
  //s->Emit(String::New("connection_error"), 1, argv); // TODO masterfix
}

static void SearchComplete(sp_search *search, void *userdata) {
  search_data_t *sdata = static_cast<search_data_t*>(userdata);
  Session *s = sdata->session;

  assert((*sdata->callback)->IsFunction());

  if (!search || sp_search_error(search) != SP_ERROR_OK) {
    Local<Value> argv[] = {
      Exception::Error(String::New(sp_error_message(sp_search_error(search))))
    };
    (*sdata->callback)->Call(s->handle_, 1, argv);
  } else {
    Handle<Value> argv[] = {
      Undefined(),
      SearchResult::New(s->session_, search)
    };
    (*sdata->callback)->Call(s->handle_, 2, argv);
  }

  cb_destroy(sdata->callback);
  delete sdata;
}

static void ImageLoadComplete(sp_image *image, void *userdata) {
 
  user_data_t *udata = static_cast<user_data_t*>(userdata);
  Session *s = udata->session;

  byte* raw;
	size_t sz = 0;
	(const void*) raw = sp_image_data(image,&sz);
	printf("Image is %lu bytes", sz);
	Local<Array> array = Array::New(sz);
	for (int i = 0; i < sz; i++) {
	    array->Set(Integer::New(i), Integer::New(raw[i]));
	}
	
	Handle<Value> argv[] = {
      Undefined(), Integer::New(sz), array
    };
    
	(*udata->callback)->Call(s->handle_, 3, argv);
	cb_destroy(udata->callback);
	sp_image_release(image);
	delete udata;
}

static void AlbumBrowseComplete(sp_albumbrowse *result, void *userdata) {
	
	user_data_t *udata = static_cast<user_data_t*>(userdata);
  	Session *s = udata->session;

	if (!result || (sp_albumbrowse_error (result) != SP_ERROR_OK)) {
		Local<Value> argv[] = {
	      Exception::Error(String::New(sp_error_message(sp_albumbrowse_error(result))))
	    };
		(*udata->callback)->Call(s->handle_, 1, argv);
		delete udata;
		return;
  	}

	sp_album* album = sp_albumbrowse_album(result);
	sp_image* image = sp_image_create(s->session_, sp_album_cover(album, SP_IMAGE_SIZE_NORMAL)); 

	if(!image /*|| (sp_image_error(image) != SP_ERROR_OK)*/) {
		Local<Value> argv[] = {
	      Exception::Error(String::New(sp_error_message(sp_image_error(image))))
	    };
	    (*udata->callback)->Call(s->handle_, 1, argv);
	} else {
		sp_image_add_load_callback (image, ImageLoadComplete, udata);	
	}
	
}


// ----------------------------------------------------------------------------
// Session implementation

Session::Session(sp_session* session)
    : session_(session)
    , main_thread_id_((pthread_t) -1)
    , login_callback_(NULL)
    , logout_callback_(NULL)
    , playlist_container_(NULL) {
  runloop_timer_ = new ev_timer;
  runloop_async_ = new ev_async;
  logmsg_async_ = new ev_async;
}

Session::~Session() {
  ev_timer_stop(EV_DEFAULT_UC_ runloop_timer_);
  ev_async_stop(EV_DEFAULT_UC_ runloop_async_);
  this->DequeueLogMessages();

  if (playlist_container_) {
    playlist_container_->Dispose();
    delete playlist_container_;
    playlist_container_ = NULL;
  }

  if (login_callback_) {
    cb_destroy(login_callback_);
    login_callback_ = NULL;
  }

  if (logout_callback_) {
    cb_destroy(logout_callback_);
    logout_callback_ = NULL;
  }

  delete runloop_timer_, runloop_async_, logmsg_async_;
}

Handle<Value> Session::New(const Arguments& args) {
  Session* s = new Session(NULL);
  static sp_session_callbacks callbacks = {
    /* logged_in */             LoggedIn,
    /* logged_out */            LoggedOut,
    /* metadata_updated */      MetadataUpdated,
    /* connection_error */      ConnectionError,
    /* message_to_user */       MessageToUser,
    /* notify_main_thread */    NotifyMainThread,
    /* music_delivery */        NULL,  // we don't play music
    /* play_token_lost */       NULL,  // we don't play music
    /* log_message */           LogMessage,
    /* end_of_track */          NULL,  // we don't play music
  };

  sp_session_config config = {
    /* api_version */           SPOTIFY_API_VERSION,
    /* cache_location */        ".spotify-cache",
    /* settings_location */     ".spotify-settings",
    /* application_key */       NULL,
    /* application_key_size */  0,
    /* user_agent */            "node-spotify",
    /* callbacks */             &callbacks,
    /* userdata */              s,
  };

  // appkey buffer
  uint8_t *application_key = NULL;

  if (args.Length() > 0) {
    if (!args[0]->IsObject())
      return JS_THROW(TypeError, "first argument must be an object");

    Local<Object> configuration = args[0]->ToObject();

    // applicationKey
    if (configuration->Has(String::New("applicationKey"))) {
      Local<Value> v = configuration->Get(String::New("applicationKey"));
      if (!v->IsArray()) {
        return JS_THROW(TypeError,
                        "applicationKey must be an array of integers");
      }
      Local<Array> a = Local<Array>::Cast(v);
      application_key = new uint8_t[a->Length()];
      config.application_key_size = a->Length();

      for (int i = 0; i < a->Length(); i++) {
        application_key[i] = a->Get(i)->Uint32Value();
      }

      config.application_key = application_key;
    }

    // userAgent
    if (configuration->Has(String::New("userAgent"))) {
      Handle<Value> v = configuration->Get(String::New("userAgent"));
      String::Utf8Value vs(v);
      config.user_agent = *vs;
    }

    // cacheLocation
    if (configuration->Has(String::New("cacheLocation"))) {
      Handle<Value> v = configuration->Get(String::New("cacheLocation"));
      String::Utf8Value vs(v);
      config.cache_location = *vs;
    }

    // settingsLocation
    if (configuration->Has(String::New("settingsLocation"))) {
      Handle<Value> v = configuration->Get(String::New("settingsLocation"));
      String::Utf8Value vs(v);
      config.settings_location = *vs;
    }
  }

  // ev_async for libspotify background thread to invoke processing on main
  s->runloop_async_->data = s;
  ev_async_init(s->runloop_async_, SpotifyRunloopAsyncProcess);
  ev_async_start(EV_DEFAULT_UC_ s->runloop_async_);
  ev_unref(EV_DEFAULT_UC); // don't let a lingering async ev keep the main loop

  // ev_timer for triggering libspotify periodic processing
  s->runloop_timer_->data = s;
  ev_timer_init(s->runloop_timer_, SpotifyRunloopTimerProcess, 60.0, 0.0);
  ev_unref(EV_DEFAULT_UC);
  // Note: No need to start the timer as it's started by first invocation after
  // NotifyMainThread

  // ev_async for libspotify background thread to emit log message on main
  s->logmsg_async_->data = s;
  ev_async_init(s->logmsg_async_, SpotifyRunloopAsyncLogMessage);
  ev_async_start(EV_DEFAULT_UC_ s->logmsg_async_);
  ev_unref(EV_DEFAULT_UC); // don't let a lingering async ev keep the main loop

  sp_session* session;
  sp_error error = sp_session_create(&config, &session);

  if (error != SP_ERROR_OK)
    return JS_THROW(Error, sp_error_message(error));

  s->session_ = session;
  s->main_thread_id_ = pthread_self();
  s->Wrap(args.Holder());
  return args.This();
}

Handle<Value> Session::Login(const Arguments& args) {
  HandleScope scope;

  if (args.Length() != 3)
    return JS_THROW(TypeError, "login takes exactly 3 arguments");
  if (!args[0]->IsString())
    return JS_THROW(TypeError, "first argument must be a string");
  if (!args[1]->IsString())
    return JS_THROW(TypeError, "second argument must be a string");
  if (!args[2]->IsFunction())
    return JS_THROW(TypeError, "last argument must be a function");

  Session* s = Unwrap<Session>(args.This());

  String::Utf8Value username(args[0]);
  String::Utf8Value password(args[1]);

  // increase refcount for our timer event
  ev_ref(EV_DEFAULT_UC);

  // save login callback
  if (s->login_callback_) cb_destroy(s->login_callback_);
  s->login_callback_ = cb_persist(args[2]);
  sp_session_login(s->session_, *username, *password, 0, NULL); 
  return Undefined();
}

Handle<Value> Session::Logout(const Arguments& args) {
  HandleScope scope;

  if (args.Length() > 0 && !args[0]->IsFunction())
    return JS_THROW(TypeError, "last argument must be a function");

  Session* s = Unwrap<Session>(args.This());

  // save logout callback
  if (args.Length() > 0) {
    if (s->logout_callback_) cb_destroy(s->logout_callback_);
    s->logout_callback_ = cb_persist(args[0]);
  }

  sp_session_logout(s->session_);
  return Undefined();
}

Handle<Value> Session::Search(const Arguments& args) {
  HandleScope scope;
  if (args.Length() != 2)
    return JS_THROW(TypeError, "search takes exactly 2 arguments");
  if (!args[0]->IsString() && !args[0]->IsObject())
    return JS_THROW(TypeError, "first argument must be a string or an object");
  if (!args[1]->IsFunction())
    return JS_THROW(TypeError, "last argument must be a function");

  Session* s = Unwrap<Session>(args.This());
  const int kDefaultTrackOffset = 0;
  const int kDefaultTrackCount = 10;
  const int kDefaultAlbumOffset = 0;
  const int kDefaultAlbumCount = 10;
  const int kDefaultArtistOffset = 0;
  const int kDefaultArtistCount = 10;
  const int kDefaultPlaylistOffset = 0;
  const int kDefaultPlaylistCount = 10;

  Handle<Value> query;
  int track_offset;
  int track_count;
  int album_offset;
  int album_count;
  int artist_offset;
  int artist_count;
  int playlist_offset;
  int playlist_count;

  if (args[0]->IsString()) {
    query = args[0];
  } else if (args[0]->IsObject()) {
    Local<Object> opt = args[0]->ToObject();
    Local<String> k = String::NewSymbol("query"); // todo: symbolize

    if (!opt->Has(k))
      return JS_THROW(TypeError, "missing required \"query\" parameter");
    query = opt->Get(k);

    #define IOPT(_name_, _intvar_, _default_)\
      k = String::New(_name_);\
      _intvar_ = opt->Has(k) ? opt->Get(k)->Uint32Value() : _default_;

    IOPT("trackOffset", track_offset, kDefaultTrackOffset);
    IOPT("trackCount", track_count, kDefaultTrackCount);
    IOPT("albumOffset", album_offset, kDefaultAlbumOffset);
    IOPT("albumCount", album_count, kDefaultAlbumOffset);
    IOPT("artistOffset", artist_offset, kDefaultArtistOffset);
    IOPT("artistCount", artist_count, kDefaultArtistCount);
    IOPT("playlistOffset", playlist_offset, kDefaultPlaylistOffset);
    IOPT("playlistCount", playlist_count, kDefaultPlaylistCount);

    #undef IOPT
  }

  search_data_t *search_data = new search_data_t;
  search_data->session = s;
  search_data->callback = cb_persist(args[1]);
  String::Utf8Value query_str(query);
  sp_search *search = sp_search_create(s->session_, *query_str,
                                       track_offset, track_count,
                                       album_offset, album_count,
                                       artist_offset, artist_count,
                                       playlist_offset, playlist_count,
                                       SP_SEARCH_STANDARD,
                                       &SearchComplete, search_data);

  if (!search)
    return JS_THROW(Error, "libspotify internal error when requesting search");

  return Undefined();
}

// .getTrackByLink( link [, callback(err, track)] ) -> Track
Handle<Value> Session::GetTrackByLink(const Arguments& args) {
  HandleScope scope;

  if (args.Length() < 1)
    return JS_THROW(TypeError, "getTrackByLink takes at least one argument");
  if (args.Length() > 1) {
    if (!args[1]->IsFunction())
      return JS_THROW(TypeError, "last argument must be a function");
  }

  Session* s = Unwrap<Session>(args.This());
  String::Utf8Value linkstr(args[0]);

  // derive sp_link from string
  sp_link *link = sp_link_create_from_string(*linkstr);
  if (!link) {
    return CallbackOrThrowError(s->handle_, args[1], "invalid link");
  }

  // derive sp_track from sp_link
  sp_track *t = sp_link_as_track(link);
  if (!t) {
    return CallbackOrThrowError(s->handle_, args[1], "not a track link");
  }

  // check status
  sp_error status = sp_track_error(t);
  if (status != SP_ERROR_IS_LOADING && status != SP_ERROR_OK) {
    return CallbackOrThrowError(s->handle_, args[1], status);
  }

  // create Track object
  Handle<Value> track = Track::New(s->session_, t);

  // "loaded" callback
  if (args.Length() > 1) {
    if (status == SP_ERROR_IS_LOADING) {
      // pending
      // todo: pass Handle<Value> instead of sp_track*
      s->metadata_update_queue_.push(args[1], t);
    } else if (status == SP_ERROR_OK) {
      // loaded
      Handle<Value> argv[] = { Undefined(), track };
      Function::Cast(*args[1])->Call(s->handle_, 2, argv);
    }
  }

  return scope.Close(track);
}

Handle<Value> Session::GetAlbumImageByLink(const Arguments& args) {
  HandleScope scope;

  if (args.Length() < 1)
    return JS_THROW(TypeError, "getAlbumByLink takes at least one argument");
  if (args.Length() > 1) {
    if (!args[1]->IsFunction())
      return JS_THROW(TypeError, "last argument must be a function");
  }

  Session* s = Unwrap<Session>(args.This());
  String::Utf8Value linkstr(args[0]);

  // derive sp_link from string
  sp_link *link = sp_link_create_from_string(*linkstr);
  if (!link) {
    return CallbackOrThrowError(s->handle_, args[1], "invalid link");
  }

  sp_album *album = sp_link_as_album(link);

  if (!album) {
    return CallbackOrThrowError(s->handle_, args[1], "not an album link");
  }

	user_data_t *udata = new user_data_t;
  	udata->session = s;
  	udata->callback = cb_persist(args[1]);

	sp_albumbrowse* albumbrowse = sp_albumbrowse_create	(s->session_,album,&AlbumBrowseComplete,udata);
  
	if (!albumbrowse)
    	return JS_THROW(Error, "libspotify internal error when requesting albumbrowse");

  	return Undefined();
}

// ---------
// Properties

Handle<Value> Session::ConnectionStateGetter(Local<String> property,
                                             const AccessorInfo& info) {
  HandleScope scope;
  Session* s = Unwrap<Session>(info.This());
  int connectionstate = sp_session_connectionstate(s->session_);
  return scope.Close(Integer::New(connectionstate));
}

Handle<Value> Session::PlaylistContainerGetter(Local<String> property,
                                               const AccessorInfo& info) {
  HandleScope scope;
  Session* s = Unwrap<Session>(info.This());

  if (!s->playlist_container_) {
    sp_playlistcontainer *pc = sp_session_playlistcontainer(s->session_);
    Handle<Value> playlist_container = PlaylistContainer::New(s->session_, pc);
    s->playlist_container_ = new Persistent<Object>();
    *s->playlist_container_ = Persistent<Object>::New(
      Handle<Object>::Cast((*playlist_container)->ToObject()));
  }

  return *s->playlist_container_;
}

Handle<Value> Session::UserGetter(Local<String> property,
                                  const AccessorInfo& info) {
  HandleScope scope;
  Session* s = Unwrap<Session>(info.This());
  sp_user* user = sp_session_user(s->session_);

  // The user property is exposed via a session object before the session
  // is connected/logged in, in which case the user object isn't initialized
  // and something weird has to be returned
  if (!user)
    return Undefined();

  return scope.Close(User::NewInstance(user));
}


void Session::Initialize(Handle<Object> target) {
  //printf("main T# %p\n", pthread_self());
  HandleScope scope;
  Local<FunctionTemplate> t = FunctionTemplate::New(New);
  t->SetClassName(String::NewSymbol("Session"));
  //t->Inherit(EventEmitter::constructor_template); // TODO masterfix

  NODE_SET_PROTOTYPE_METHOD(t, "logout", Logout);
  NODE_SET_PROTOTYPE_METHOD(t, "login", Login);
  NODE_SET_PROTOTYPE_METHOD(t, "search", Search);
  NODE_SET_PROTOTYPE_METHOD(t, "getTrackByLink", GetTrackByLink);
  NODE_SET_PROTOTYPE_METHOD(t, "getAlbumImageByLink", GetAlbumImageByLink);

  Local<ObjectTemplate> instance_t = t->InstanceTemplate();
  instance_t->SetInternalFieldCount(1);
  instance_t->SetAccessor(String::New("user"), UserGetter);
  instance_t->SetAccessor(String::New("_connectionState"),
                          ConnectionStateGetter);
  instance_t->SetAccessor(String::New("playlists"), PlaylistContainerGetter);

  target->Set(String::New("Session"), t->GetFunction());

  log_message_symbol = NODE_PSYMBOL("logMessage");
}
