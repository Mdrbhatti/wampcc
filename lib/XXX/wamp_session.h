#ifndef XXX_SESSION_H
#define XXX_SESSION_H

#include "XXX/types.h"
#include "XXX/io_listener.h"
#include "XXX/protocol.h"

#include <jalson/jalson.h>

#include <map>
#include <mutex>
#include <memory>
#include <future>
#include <set>

namespace XXX {

  class protocol;
  class wamp_session;
  class kernel;
  class pubsub_man;
  class tcp_socket;
  struct logger;

  typedef std::function< void(wamp_args, std::unique_ptr<std::string> ) > wamp_invocation_reply_fn;
  typedef std::function< void(std::weak_ptr<wamp_session>, bool) > session_state_fn;

  /** Handler interface for server-side authentication.  An instance of
   * auth_provider must be provided to each server-side wamp_session, which
   * allows that session to perform authentication with the peer. */
  struct auth_provider
  {
    enum t_auth_required
    {
      e_forbidden,    /* user not permitted */
      e_open,         /* user allowed without authentication */
      e_authenticate  /* user must authenticate */
    };

    /* auth_plan combines auth requirement plus list of supported methods */
    typedef std::tuple<t_auth_required, std::set<std::string>> auth_plan;

    std::function<std::string(const std::string& realm)> provider_name;

    /* Deal with request for user access to realm. Must return the
     * authentication requirement and a set of supported authentication
     * methods. */
    std::function<
      auth_plan
      (const std::string& user, const std::string& realm) > permit_user_realm;

    std::function<std::string(const std::string& user, const std::string& realm)> get_user_secret;
  };

  struct server_msg_handler
  {
    std::function<void(wamp_session*, std::string, wamp_args, wamp_invocation_reply_fn)> inbound_call;
    std::function<void(wamp_session*, std::string uri, jalson::json_object, wamp_args)> handle_inbound_publish;
    std::function<uint64_t (std::weak_ptr<wamp_session>, std::string realm, std::string uri)> inbound_register;
    std::function<uint64_t (wamp_session*, t_request_id, std::string uri, jalson::json_object&)> inbound_subscribe;
    std::function<void (wamp_session*, t_request_id, t_subscription_id)> inbound_unsubscribe;
  };

  struct client_credentials
  {
    std::string realm;
    std::string authid;
    std::vector< std::string > authmethods;
    std::function< std::string() > secret_fn;
  };


  struct wamp_subscription_event
  {
    t_subscription_id subscription_id;
    jalson::json_object details;
    wamp_args args;
    void* user;
  };
  typedef std::function< void (wamp_subscription_event) > subscription_event_cb;

  /** Callback invoked when a subscribe request is successful or fails. Error
      contains the error code when the subscription is not successful.
  */
  typedef std::function< void (t_request_id,
                               std::string uri,
                               bool success,
                               t_subscription_id,
                               std::string error) > subscribed_cb;

  /** Callback invoked when an unsubscription request completes. Error contains
      the error code when the subscription is not successful.
  */
  typedef std::function< void (t_request_id,
                               bool success,
                               std::string error) > unsubscribed_cb;

  struct wamp_call_result
  {
    t_request_id reqid;    /* protocol ID that was used */
    std::string procedure;
    bool was_error;
    std::string error_uri; // if was_error == true
    jalson::json_object details;
    wamp_args args;
    void * user;

    wamp_call_result()
      : reqid(0),
        was_error(false),
        user(0){}
  };

  typedef std::function< void (wamp_call_result) > wamp_call_result_cb;


  /** Aggregate passed on RPC invocation. */
  struct wamp_invocation
  {
    jalson::json_array  arg_list;
    jalson::json_object arg_dict;
    jalson::json_object details;
    void *              user;

    std::function<void(jalson::json_array, jalson::json_object)> yield;
    std::function<void(std::string, jalson::json_array, jalson::json_object)> error;
  };

  typedef std::function<void(wamp_invocation&) > rpc_cb;

  class wamp_error : public std::runtime_error
  {
  public:
    wamp_error(const char* error_uri, const char* what, wamp_args wa = wamp_args())
      : std::runtime_error(what),
        m_uri(error_uri),
        m_args(wa)
    {  }

    wamp_error(const char* error_uri, wamp_args wa = wamp_args())
      : std::runtime_error(error_uri),
        m_uri(error_uri),
        m_args(wa)
    {  }

    wamp_args& args() { return m_args; }
    const wamp_args& args() const { return m_args; }

    const std::string & error_uri() { return m_uri; }

  private:
    std::string m_uri;
    wamp_args m_args;
  };


  // Needs to support needs of service providers (rpc & topics), and service
  // consumers (rpc callers, and subscribers)
  class wamp_session : public std::enable_shared_from_this<wamp_session>,
                       public io_listener
  {
  public:

    struct options
    {
      std::chrono::milliseconds max_pending_open { 10000 };
    };

    enum class t_session_mode {client, server};

    /** Create a server side session (i.e., the socket was accepted from a
     * remote client). */
    static std::shared_ptr<wamp_session> create(kernel*,
                                                std::unique_ptr<tcp_socket>,
                                                session_state_fn,
                                                protocol_builder_fn ,
                                                server_msg_handler,
                                                auth_provider);

    /** Create a client side session (i.e., the socket was actively connected to
     * a remote server) using a protocol class as specified via the template
     * parameter. */
    template<typename T>
    static std::shared_ptr<wamp_session> create(kernel* k,
                                                std::unique_ptr<tcp_socket> socket,
                                                session_state_fn state_cb,
                                                typename T::options protocol_options)
    {
      protocol_builder_fn factory_fn;
      factory_fn = [protocol_options](tcp_socket* socket,
                                      protocol::t_msg_cb _msg_cb,
                                      protocol::protocol_callbacks callbacks)
        {
          std::unique_ptr<protocol> up (
            new T(socket, _msg_cb, callbacks,
                  protocol::connection_mode::eActive, protocol_options)
            );
          return up;
        };

      return wamp_session::create_impl(k, t_session_mode::client, std::move(socket),
                                       state_cb, factory_fn, server_msg_handler(), auth_provider());
    }

    /** Should be called client session once the session has been created, to
     * begin the HELLO sequence. */
    std::future<void> initiate_hello(client_credentials);

    ~wamp_session();

    /** Request asynchronous graceful session close */
    std::shared_future<void> close();

    /** Perform synchronous fast (ungraceful) session close */
    void fast_close();

    session_handle handle() { return shared_from_this(); }

    bool is_open() const;
    bool is_closed() const;
    bool is_pending_open() const;

    /** Number of seconds since session constructed  */
    int duration_since_creation() const;

    /** Time since last message */
    int duration_since_last() const;

    /** Does this session use heartbeats? */
    bool uses_heartbeats() const;

    /** Return the realm, or empty string if a realm has not yet been provided,
     * eg, in case of a passive session that receives the realm from remote
     * peer. */
    const std::string& realm() const;

    int hb_interval_secs() const { return m_hb_intvl; }

    t_request_id provide(std::string uri,
                         const jalson::json_object& options,
                         rpc_cb cb,
                         void * data = nullptr);

    /** Subscribe to a topic. The subscribed_cb callback is invoked upon success
     * or failure of the request. Subsequent topic updates which can follow a
     * successful subscription are delivered via the subscription_event_cb
     * callback.
     *
     * Note that while unadvised, a topic can be subscribed to more than once.
     * Doing so does not multiply the subsequent topic events, however, it is
     * the event-callback associated with the most recent subscription that is
     * used to deliver topic events.
     */
    t_request_id subscribe(std::string uri,
                           jalson::json_object options,
                           subscribed_cb,
                           subscription_event_cb cb,
                           void * user = nullptr);

    /** Unsubscribe a subscription. The subscription is identified via its WAMP
     * subscription ID.  The unsubscribed_cb callback is invoked upon success or
     * failure of the request. */
    t_request_id unsubscribe(t_subscription_id,
                             unsubscribed_cb,
                             void * user = nullptr);

    t_request_id call(std::string uri,
                      const jalson::json_object& options,
                      wamp_args args,
                      wamp_call_result_cb user_cb,
                      void* user_data = nullptr);

    t_request_id publish(std::string uri,
                         const jalson::json_object& options,
                         wamp_args args);

    t_request_id invocation(uint64_t registration_id,
                            const jalson::json_object& options,
                            wamp_args args,
                            wamp_invocation_reply_fn);

    t_sid unique_id() const { return m_sid; }

    t_session_mode session_mode() const { return m_session_mode; }

    const char* protocol_name() const { return m_proto->name(); }

    std::shared_future<void>  closed_future() const { return m_shfut_has_closed; }
    std::shared_future<void>& closed_future()       { return m_shfut_has_closed; }

  private:;

    static std::shared_ptr<wamp_session> create_impl(kernel*,
                                                     t_session_mode,
                                                     std::unique_ptr<tcp_socket>,
                                                     session_state_fn,
                                                     protocol_builder_fn ,
                                                     server_msg_handler,
                                                     auth_provider);

    wamp_session(kernel*,
                 t_session_mode,
                 std::unique_ptr<tcp_socket>,
                 session_state_fn state_cb,
                 server_msg_handler,
                 auth_provider);

    wamp_session(const wamp_session&) = delete;
    wamp_session& operator=(const wamp_session&) = delete;

    void io_on_read(char*, ssize_t) override;
    void decode_and_process(char*, size_t len);
    void process_message(unsigned int, jalson::json_array&);
    void handle_exception();

    void update_state_for_outbound(const jalson::json_array& msg);

    void send_msg(jalson::json_array&, bool final=false);

    void upgrade_protocol(std::unique_ptr<protocol>&);

    friend class tcp_socket;
    friend class pubsub_man;

    enum SessionState
    {
      eInit               = (1<<1),

      eRecvHello          = (1<<2),  //
      eSentChallenge      = (1<<3),  //  server only
      eRecvAuth           = (1<<4),  //

      eSentHello          = (1<<5),  //
      eRecvChallenge      = (1<<6),  //  client only
      eSentAuth           = (1<<7),  //

      eOpen               = (1<<8),
      e_wait_peer_goodbye = (1<<9),
      eClosing            = (1<<10),
      eClosed             = (1<<11)
    } m_state;
    mutable std::mutex m_state_lock;

    void change_state(unsigned expected, SessionState next);
    void initiate_close(std::lock_guard<std::mutex>&);
    void transition_to_closed();

    void handle_HELLO(jalson::json_array& ja);
    void handle_CHALLENGE(jalson::json_array& ja);
    void handle_AUTHENTICATE(jalson::json_array& ja);
    void send_WELCOME();

    void notify_session_open();
    static const char* state_to_str(wamp_session::SessionState);

    logger & __logger; /* name chosen for log macros */
    kernel* m_kernel;

    uint64_t m_sid;
    std::unique_ptr< tcp_socket> m_socket;

    t_session_mode m_session_mode;

    std::promise<void> m_has_closed;
    std::shared_future<void> m_shfut_has_closed;

    /* Interval, in secs, at which to send heartbeats. Values below 30 seconds
        might not be too reliable, because the underlying housekeeping timer has
        around a 20 second precision. */
    int m_hb_intvl;
    time_t m_time_create;

    time_t m_time_last_msg_recv;

    mutable std::mutex m_request_lock;
    t_request_id m_next_request_id;

    std::function< std::string() > m_client_secret_fn;

    std::string m_realm;
    std::string m_authid;
    std::string m_challenge;
    mutable std::mutex m_realm_lock;

    auth_provider m_auth_proivder;
    bool m_server_requires_auth;

    session_state_fn m_notify_state_change_fn;
    std::weak_ptr<wamp_session> m_self_weak;

    void process_inbound_registered(jalson::json_array &);
    void process_inbound_invocation(jalson::json_array &);
    void process_inbound_subscribed(jalson::json_array &);
    void process_inbound_unsubscribed(jalson::json_array &);
    void process_inbound_event(jalson::json_array &);
    void process_inbound_result(jalson::json_array &);
    void process_inbound_error(jalson::json_array &);
    void process_inbound_call(jalson::json_array &);
    void process_inbound_yield(jalson::json_array &);
    void process_inbound_publish(jalson::json_array &);
    void process_inbound_subscribe(jalson::json_array &);
    void process_inbound_unsubscribe(jalson::json_array &);
    void process_inbound_register(jalson::json_array &);
    void process_inbound_goodbye(jalson::json_array &);
    void process_inbound_abort(jalson::json_array &);

    void invocation_yield(int request_id,
                          wamp_args args);

    void reply_with_error(int request_type,
                          int request_id,
                          wamp_args args,
                          std::string error_uri);

    jalson::json_array build_goodbye_message(std::string);
    jalson::json_array build_abort_message(std::string);

    void drop_connection(std::string);
    void drop_connection_impl(std::string, std::lock_guard<std::mutex>&);

    bool user_cb_allowed() const { return m_state != eClosed; }

    server_msg_handler m_server_handler;

    struct procedure
    {
      std::string uri;
      rpc_cb user_cb;
      void * user_data;
    };

    struct subscribe_request
    {
      std::string uri;
      subscribed_cb request_cb;
      subscription_event_cb event_cb;
      void * user_data;
    };

    struct unsubscribe_request
    {
      unsubscribed_cb request_cb;
      t_subscription_id subscription_id;
      void * user_datax;
    };

    struct subscription
    {
      subscription_event_cb event_cb;
      void * user_data;
    };

    struct wamp_call
    {
      std::string rpc;
      wamp_call_result_cb user_cb;
      void* user_data;
      wamp_call() : user_data( nullptr ) { }
    };

    struct wamp_invocation
    {
      wamp_invocation_reply_fn reply_fn;
    };

    mutable std::mutex m_pending_lock;
    std::map<t_request_id, subscribe_request>   m_pending_subscribe;
    std::map<t_request_id, unsubscribe_request> m_pending_unsubscribe;
    std::map<t_request_id, procedure>           m_pending_register;
    std::map<t_request_id, wamp_call>           m_pending_call;
    std::map<t_request_id, wamp_invocation>     m_pending_invocation;

    // TODO: procedures -- not currently locked, however, need to add locking once
    // unprovide() is added, and if it is implemented synchronously.
    std::map<t_request_id, procedure> m_procedures;

    // No locking required, since used only on EV thread
    std::map<t_subscription_id, subscription> m_subscriptions;

    std::unique_ptr<protocol> m_proto;

    std::promise< void > m_promise_on_open;

    options m_options;
  };

} // namespace XXX

#endif
