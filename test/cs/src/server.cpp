#include <netp.hpp>
//#include <vld.h>

//using namespace wawo;
//using namespace netp::net;

class example_handler :
	public netp::channel_handler_abstract
{
public:
	example_handler() :
		channel_handler_abstract(netp::CH_ACTIVITY|netp::CH_INBOUND_READ|netp::CH_OUTBOUND_WRITE)
	{}

	void read(NRP<netp::channel_handler_context> const& ctx, NRP<netp::packet> const& income) override
	{
		NETP_INFO("<<<: %u bytes", income->len() );
		ctx->fire_read(income);
	}

	void write(NRP<netp::channel_handler_context> const& ctx, NRP<netp::packet> const& outlet, NRP<netp::promise<int>> const& ch_promise)  override
	{
		NETP_INFO(">>>: %u bytes", outlet->len() );
		ctx->write(outlet,ch_promise);
	}
	
	void connected(NRP<netp::channel_handler_context> const& ctx)  override  {
		NETP_INFO("connected: %d", ctx->ch->ch_id() );
		ctx->fire_connected();
	}

	void closed(NRP<netp::channel_handler_context> const& ctx)  override {
		NETP_INFO("closed: %d", ctx->ch->ch_id());
		ctx->fire_closed();
	}	

	void read_closed(NRP<netp::channel_handler_context> const& ctx)  override {
		NETP_INFO("read_shutdowned: %d", ctx->ch->ch_id() );
		ctx->close_write();
		ctx->fire_read_closed();
	}
	void write_closed(NRP<netp::channel_handler_context> const& ctx) override {
		NETP_INFO("write_shutdowned: %d", ctx->ch->ch_id());
		ctx->close_read();
		ctx->fire_write_closed();
	}

	void write_block(NRP<netp::channel_handler_context> const& ctx)  override {
		NETP_INFO("write_block: %d", ctx->ch->ch_id());
		ctx->fire_write_block();
	}

	void write_unblock(NRP<netp::channel_handler_context> const& ctx) override {
		NETP_INFO("write_unblock: %d", ctx->ch->ch_id());
		ctx->fire_write_unblock();
	}
};


class my_echo :
	public netp::channel_handler_abstract
{
public:
	my_echo() :
		channel_handler_abstract(netp::CH_ACTIVITY_READ_CLOSED | netp::CH_INBOUND_READ)
	{}
		~my_echo() {}
	void read(NRP<netp::channel_handler_context> const& ctx, NRP<netp::packet> const& income) override {
		 NRP<netp::future<int>> ch_future = ctx->write(income);

		ch_future->add_listener([]( NRP<netp::future<int>> const& ch_future) {
			if (ch_future->is_success()) {
				int wrt = ch_future->get();
				NETP_INFO("write rt: %d", wrt);
			}
		});
	}

	void read_closed(NRP<netp::channel_handler_context> const& ctx) override {
		ctx->close();
	}
};

int main(int argc, char** argv) {

	int* p = new int(3);//vld check
	netp::app app;

	NRP<netp::channel_future> f_listen = netp::socket::listen_on( "tcp://0.0.0.0:22310", [](NRP<netp::channel> const& ch) {
		ch->pipeline()->add_last(netp::make_ref<example_handler>());
		ch->pipeline()->add_last(netp::make_ref<my_echo>());
	});

	int listenrt = f_listen->get();
	if (listenrt != netp::OK) {
		return listenrt;
	}

	app.run();
	f_listen->channel()->ch_close();
	f_listen->channel()->ch_close_future()->wait();

	NETP_ASSERT(f_listen->channel()->ch_close_future()->is_done());
	NETP_INFO("lsocket closed close: %d", f_listen->channel()->ch_close_future()->get());

	return netp::OK;
}