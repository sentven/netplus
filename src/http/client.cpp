
#include <netp/http/client.hpp>
#include <netp/socket.hpp>

#include <netp/handler/dump_in_text.hpp>
#include <netp/handler/dump_out_text.hpp>

namespace netp { namespace http {

	enum client_activity {
		act_connected = -1,
		act_error = -2
	};

	typedef std::function<void(int err)> fn_http_client_activity_err_t;
	typedef std::function<void(NRP<client> const&)> fn_http_client_activity_connected_t;

	NRP<netp::promise<int>> client::close() {
		NRP<netp::promise<int>> closef = netp::make_ref<netp::promise<int>>();
		m_loop->execute([C = NRP<client>(this), closef]() {
			C->_do_close(closef);
		});
		return closef;
	}

	void client::_do_request_done(int code, NRP<netp::http::message> const& r) {
		NETP_ASSERT(m_loop->in_event_loop());

		if( m_reqs.size() == 0) {
			NETP_ASSERT(m_mtmp == nullptr );
			NETP_INFO("[client]no request in queue, code: %d", code );
			return ;
		}

		NRP<http_request_ctx> rctx = m_reqs[0];
		m_reqs.pop_back();
		NETP_ASSERT(m_reqs.size() == 0);

		rctx->state = http_request_state::S_DONE;
		if ((rctx->reqp->nbytes_notified>0)) {
			NETP_ASSERT(r == nullptr);
			rctx->reqp->fn_body(nullptr, -1);
		}
		NETP_ASSERT(r != nullptr);
		rctx->reqp->set(std::make_tuple(code, r ));
	}

	void client::do_request(NRP<netp::http::message> const& m, NRP<netp::http::request_promise> const& reqp, std::chrono::seconds timeout) {

		if (!m_loop->in_event_loop()) {
			m_loop->execute([C = NRP<client>(this), m, reqp, timeout]() {
				C->do_request(m, reqp, timeout);
			});
			return;
		}

		if (m_wstate == http_write_state::S_WRITE_CLOSED) {
			reqp->set(std::make_tuple(netp::E_CHANNEL_WRITE_CLOSED,nullptr));
			return;
		}

		//we donot support pipeline for now
		if (m_reqs.size()) {
			reqp->set(std::make_tuple(netp::E_HTTP_CLIENT_REQ_IN_OP,nullptr));
			return;
		}

		NETP_ASSERT(m->opt != O_NONE);
		m->type = T_REQ;
		if (m->H == nullptr) {
			m->H = netp::make_ref<header>();
		}

		m->ver = { 1,1 };
		if (!m->H->have("host")) {
			m->H->set("Host", m_host);
		}

		netp::http::url_fields fields;
		netp::http::parse_url(m->url.c_str(), m->url.length(), m->urlfields);

		if (!m->H->have("user-agent")) {
			m->H->set("User-Agent", __NETP_VER_STR);
		}

		NRP<netp::packet> outp;
		m->encode(outp);

		NRP<http_request_ctx> ctx = netp::make_ref<http_request_ctx>();
		ctx->state = http_request_state::S_REQUESTING;
		ctx->reqm = m;
		ctx->reqp = reqp;
		ctx->writep = netp::make_ref<netp::promise<int>>();
		m_reqs.push_back(ctx);

		ctx->writep->if_done([L = m_loop, reqp, C = NRP<client>(this)](int const& rt) {
			NETP_ASSERT(L->in_event_loop());
			if (rt != netp::OK) {
				C->_do_request_done(rt,nullptr);
			}
		});

		m_ctx->write(outp, ctx->writep);
		NRP<netp::timer> tm_REQ = netp::make_ref<netp::timer>(timeout, [C = NRP<client>(this), ctx](NRP<netp::timer> const& tm) {
			if (ctx->state == http_request_state::S_REQUESTING) {
				NETP_WARN("[client]http req timeout, url: %s", ctx->reqm->url.c_str());
				C->_do_request_done(netp::E_HTTP_REQ_TIMEOUT,nullptr);
				C->close();
			}
			(void)tm;
		});
		m_loop->launch(tm_REQ, netp::make_ref<promise<int>>());
	}

	void client::_do_close(NRP<netp::promise<int>> const& close_f) {
		NETP_ASSERT(m_loop->in_event_loop());
		m_ctx->close(close_f);
	}

	void client::http_cb_connected(NRP<netp::channel_handler_context> const& ctx_) {
		NETP_ASSERT(m_loop->in_event_loop());
		NETP_ASSERT(m_ctx == nullptr);
		m_ctx = ctx_;
		m_wstate = http_write_state::S_WRITE_IDLE;
		m_close_f = netp::make_ref<promise<int>>();
		event_broker_any::invoke<fn_http_client_activity_connected_t>(act_connected, NRP<client>(this));
		event_broker_any::unbind(act_connected);
		event_broker_any::unbind(act_error);
	}

	void client::http_cb_closed(NRP<netp::channel_handler_context> const& ctx_) {
		NETP_ASSERT(m_loop->in_event_loop());
		_do_request_done(netp::E_HTTP_REQ_TIMEOUT,nullptr);
		m_mtmp = nullptr;
		m_ctx = nullptr;
		m_close_f->set( netp::OK );
		(void)ctx_;
	}

	void client::http_write_closed(NRP<netp::channel_handler_context> const& ctx_) {
		NETP_ASSERT(m_loop->in_event_loop());
		m_wstate = http_write_state::S_WRITE_CLOSED;
		(void)ctx_;
	}

	void client::http_cb_error(NRP<netp::channel_handler_context> const& ctx_, int err) {
		(void)ctx_;
		event_broker_any::invoke<fn_http_client_activity_err_t>(act_error,err);
		event_broker_any::unbind(act_connected);
		event_broker_any::unbind(act_error);
	}

	//void client::http_cb_parse_error(NRP<netp::channel_handler_context> const& ctx_, int err) {
	//	NETP_ASSERT(!"parse erorr");
	//}

	void client::http_cb_message_header(NRP<netp::channel_handler_context> const& ctx_, NRP<netp::http::message> const& m) {
		NETP_ASSERT(m_loop->in_event_loop());
		if (m_reqs.size() == 0) {
			NETP_INFO("[client]no request found for http body");
			return;
		}

		if (m_reqs[0]->reqp->fn_header != nullptr) {
			m_reqs[0]->reqp->fn_header(m_mtmp);
			m_mtmp = nullptr;
			return;
		}

		m_mtmp = m;
		(void)ctx_;
	}

	void client::http_cb_message_body(NRP<netp::channel_handler_context> const& ctx_, const char* data, u32_t len) {
		NETP_ASSERT(m_loop->in_event_loop());
		if (m_reqs.size() == 0) {
			NETP_INFO("[client]no request found for http body");
			return;
		}

		if (m_reqs[0]->reqp->fn_body != nullptr) {
			NETP_ASSERT(m_mtmp == nullptr);
			m_reqs[0]->reqp->nbytes_notified += len;
			m_reqs[0]->reqp->fn_body((char*)data, len);
			return;
		}

		NETP_ASSERT(m_mtmp != nullptr);
		if (m_mtmp->body == nullptr) {
			m_mtmp->body = netp::make_ref<packet>((byte_t*)data,len);
		} else {
			m_mtmp->body->write((byte_t*)data, len);
		}
		(void)ctx_;
	}

	void client::http_cb_message_end(NRP<netp::channel_handler_context> const& ctx_) {
		NETP_ASSERT(m_loop->in_event_loop());
		_do_request_done(netp::OK, m_mtmp);
		(void)ctx_;
	}

	void do_dial(const char* host, size_t len, NRP<client_dial_promise> const& dp, dial_cfg const& dcfg) {
		if(host == 0 || len == 0) {
			dp->set(std::make_tuple(netp::E_HTTP_INVALID_HOST, nullptr));
			return ;
		}

		netp::http::url_fields fields;
		int rt = netp::http::parse_url(host,len, fields);
		if (rt != netp::OK) {
			dp->set(std::make_tuple(netp::E_HTTP_INVALID_HOST, nullptr));
			return ;
		}

		bool is_https = netp::iequals<string_t>(fields.schema, "https");
		string_t http_host = fields.host + string_t(":") + (netp::to_string(fields.port));
		string_t dial_url = string_t("tcp://") + http_host;
		NRP<channel_dial_promise> ch_dp = netp::make_ref<channel_dial_promise>();
		ch_dp->if_done([dp](std::tuple<int, NRP<channel>> const& tupc) {
			if (std::get<0>(tupc) != netp::OK) {
				dp->set(std::make_tuple(std::get<0>(tupc), nullptr));
			}
		});

		auto fn_connected = [dp](NRP<client> const& c_) {
			dp->set(std::make_tuple(netp::OK, c_));
		};
		auto fn_dial_error = [dp](int err) {
			dp->set(std::make_tuple(err, nullptr));
		};

		auto fn_initializer = [fn_connected, fn_dial_error, is_https, dcfg, http_host](NRP<channel> const& ch) {

#ifdef NETP_ENABLE_TLS
			NETP_TODO("to impl");
			if (is_https && dcfg.tls.cert.length() ) {
				NETP_TODO("we have to set a default tls_context for https");
			}

			if (dcfg.tls.cert.length() ) {
				ch->pipeline()->add_last(netp::make_ref<netp::handler::tls>(dcfg.tls_ctx));
			}
#else
			if (dcfg.tls.cert.length()) {
				NETP_ASSERT(!"do not supported yet");
			}
#endif

#ifndef NETP_ENABLE_TRACE_HTTP_MESSAGE
			if (dcfg.dump_in) {
#endif

				ch->pipeline()->add_last(netp::make_ref<netp::handler::dump_in_text>());
#ifndef NETP_ENABLE_TRACE_HTTP_MESSAGE
			}
#endif

#ifndef NETP_ENABLE_TRACE_HTTP_MESSAGE
			if (dcfg.dump_out) {
#endif
				ch->pipeline()->add_last(netp::make_ref<netp::handler::dump_out_text>());
#ifndef NETP_ENABLE_TRACE_HTTP_MESSAGE
			}
#endif

			NRP<netp::handler::http> httph = netp::make_ref<netp::handler::http>();
			NRP<netp::http::client> c = netp::make_ref<client>(http_host.c_str(), http_host.length(), ch->L);

			c->bind<fn_http_client_activity_connected_t>(act_connected, fn_connected);
			c->bind<fn_http_client_activity_err_t>(act_error, fn_dial_error);

			httph->bind<netp::handler::http::fn_http_activity_t>(netp::handler::http::E_CONNECTED, &client::http_cb_connected, c, std::placeholders::_1);
			httph->bind<netp::handler::http::fn_http_activity_t>(netp::handler::http::E_CLOSED, &http::client::http_cb_closed, c, std::placeholders::_1);
			httph->bind<netp::handler::http::fn_http_activity_t>(netp::handler::http::E_WRITE_CLOSED, &http::client::http_write_closed, c, std::placeholders::_1);
			httph->bind<netp::handler::http::fn_http_error_t>(netp::handler::http::E_ERROR, &http::client::http_cb_error, c, std::placeholders::_1, std::placeholders::_2);

			httph->bind<netp::handler::http::fn_http_message_header_t>(netp::handler::http::E_MESSAGE_HEADER, &http::client::http_cb_message_header, c, std::placeholders::_1, std::placeholders::_2);
			httph->bind<netp::handler::http::fn_http_message_body_t>(netp::handler::http::E_MESSAGE_BODY, &http::client::http_cb_message_body, c, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
			httph->bind<netp::handler::http::fn_http_message_end_t>(netp::handler::http::E_MESSAGE_END, &http::client::http_cb_message_end, c, std::placeholders::_1);

			ch->pipeline()->add_last(httph);
		};

		netp::socket::do_dial(dial_url.c_str(), dial_url.length(), fn_initializer, ch_dp, dcfg.cfg);
	}
	void do_dial(std::string const& host, NRP<client_dial_promise> const& dp, dial_cfg const& dcfg) {
		do_dial(host.c_str(), host.length(), dp, dcfg);
	}

	NRP<client_dial_promise> dial(const char* host, size_t len, dial_cfg const& dcfg) {
		NRP<client_dial_promise> dp = netp::make_ref<client_dial_promise>();
		do_dial(host,len, dp, dcfg );
		return dp;
	}

	NRP<client_dial_promise> dial(std::string const& host, dial_cfg const& dcfg) {
		return dial(host.c_str(), host.length(), dcfg);
	}

	void do_get(std::string const& url, NRP<netp::http::request_promise> const& reqp, std::chrono::seconds timeout) {
		NRP<client_dial_promise> dp = netp::http::dial(url, { true,true,{},netp::make_ref<netp::socket_cfg>() });
		dp->if_done([url,reqp,timeout]( std::tuple<int, NRP<client>> const& tupc ) {
			int dialrt = std::get<0>(tupc);
			if(dialrt != netp::OK) {
				reqp->set(std::make_tuple(dialrt,nullptr));
				return;
			}
			std::get<1>(tupc)->do_get(url, reqp, timeout);
		});
	}

	NRP<netp::http::request_promise> get(std::string const& url, std::chrono::seconds timeout) {
		NRP<request_promise> rp = netp::make_ref<request_promise>();
		do_get(url, rp, timeout);
		return rp;
	}

}}