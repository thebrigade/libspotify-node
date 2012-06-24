#ifndef SPOTIFY_SESSION_H_
#define SPOTIFY_SESSION_H_

#include "index.h"
#include "callback_queue.h"
#include "playlistcontainer.h"

#include <queue>

class Session : public node::ObjectWrap {
 public:
  static void Initialize(v8::Handle<v8::Object> target);

  static v8::Handle<v8::Value> New(const v8::Arguments& args);
  static v8::Handle<v8::Value> Login(const v8::Arguments& args);
  static v8::Handle<v8::Value> Logout(const v8::Arguments& args);
  static v8::Handle<v8::Value> Search(const v8::Arguments& args);
  static v8::Handle<v8::Value> GetTrackByLink(const v8::Arguments& args);
  static v8::Handle<v8::Value> GetAlbumImageByLink(const v8::Arguments& args);
  static v8::Handle<v8::Value> ConnectionStateGetter(
      v8::Local<v8::String> property,
      const v8::AccessorInfo& info);
  static v8::Handle<v8::Value> PlaylistContainerGetter(
      v8::Local<v8::String> property,
      const v8::AccessorInfo& info);

  // Gets the user associated with a session
  static v8::Handle<v8::Value> UserGetter(v8::Local<v8::String> property,
                                          const v8::AccessorInfo& info);

  void ProcessEvents();

  explicit Session(sp_session* session);
  ~Session();

  sp_session* session_;
  pthread_t main_thread_id_;
  v8::Persistent<v8::Function> *logout_callback_;
  v8::Persistent<v8::Function> *login_callback_;
  v8::Persistent<v8::Object> *playlist_container_;

  // Node-Spotify runloop glue
  ev_timer* runloop_timer_;
  ev_async* runloop_async_;

  // Spotify background thread-to-node-main glue
  ev_async* logmsg_async_;

  // Log messages delivered from a background thread
  std::queue<const char*> log_message_queue_;

  // queued callbacks waiting for metadata_update events for particular objects
  CallbackQueue  metadata_update_queue_;

  void DequeueLogMessages();
};

#endif  // SPOTIFY_SESSION_H_
