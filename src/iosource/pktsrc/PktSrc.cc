// See the file "COPYING" in the main distribution directory for copyright.

#include <errno.h>
#include <sys/stat.h>

#include "config.h"

#include "util.h"
#include "PktSrc.h"
#include "Hash.h"
#include "Net.h"
#include "Sessions.h"

using namespace iosource;

PktSrc::PktSrc()
	{
	have_packet = false;
	errbuf = "";

	next_sync_point = 0;
	first_timestamp = 0.0;
	first_wallclock = current_wallclock = 0;
	}

PktSrc::~PktSrc()
	{
	}

const std::string& PktSrc::Path() const
	{
	static std::string not_open("not open");
	return IsOpen() ? props.path : not_open;
	}

const char* PktSrc::ErrorMsg() const
	{
	return errbuf.c_str();
	}

int PktSrc::LinkType() const
	{
	return IsOpen() ? props.link_type : -1;
	}

int PktSrc::HdrSize() const
	{
	return IsOpen() ? props.hdr_size : -1;
	}

int PktSrc::SnapLen() const
	{
	return snaplen; // That's a global. Change?
	}

bool PktSrc::IsLive() const
	{
	return props.is_live;
	}

double PktSrc::CurrentPacketTimestamp()
	{
	return current_pseudo;
	}

double PktSrc::CurrentPacketWallClock()
	{
	// We stop time when we are suspended.
	if ( net_is_processing_suspended() )
		current_wallclock = current_time(true);

	return current_wallclock;
	}

void PktSrc::Opened(const Properties& arg_props)
	{
	props = arg_props;
	SetClosed(false);

	DBG_LOG(DBG_PKTIO, "Opened source %s", props.path.c_str());
	}

void PktSrc::Closed()
	{
	SetClosed(true);

	DBG_LOG(DBG_PKTIO, "Closed source %s", props.path.c_str());
	}

void PktSrc::Error(const std::string& msg)
	{
	// We don't report this immediately, Bro will ask us for the error
	// once it notices we aren't open.
	errbuf = msg;
	DBG_LOG(DBG_PKTIO, "Error with source %s: %s",
		IsOpen() ? props.path.c_str() : "<not open>",
		msg.c_str());
	}

void PktSrc::Info(const std::string& msg)
	{
	reporter->Info("%s", msg.c_str());
	}

void PktSrc::Weird(const std::string& msg, const Packet* p)
	{
	sessions->Weird(msg.c_str(), p->hdr, p->data, 0);
	}

void PktSrc::InternalError(const std::string& msg)
	{
	reporter->InternalError("%s", msg.c_str());
	}

void PktSrc::ContinueAfterSuspend()
	{
	current_wallclock = current_time(true);
	}

int PktSrc::GetLinkHeaderSize(int link_type)
	{
	switch ( link_type ) {
	case DLT_NULL:
		return 4;

	case DLT_EN10MB:
		return 14;

	case DLT_FDDI:
		return 13 + 8;	// fddi_header + LLC

#ifdef DLT_LINUX_SLL
	case DLT_LINUX_SLL:
		return 16;
#endif

	case DLT_PPP_SERIAL:	// PPP_SERIAL
		return 4;

	case DLT_RAW:
		return 0;
	}

	return -1;
	}

double PktSrc::CheckPseudoTime()
	{
	if ( ! IsOpen() )
		return 0;

	if ( ! ExtractNextPacketInternal() )
		return 0;

	if ( remote_trace_sync_interval )
		{
		if ( next_sync_point == 0 || current_packet.ts >= next_sync_point )
			{
			int n = remote_serializer->SendSyncPoint();
			next_sync_point = first_timestamp +
						n * remote_trace_sync_interval;
			remote_serializer->Log(RemoteSerializer::LogInfo,
				fmt("stopping at packet %.6f, next sync-point at %.6f",
					current_packet.ts, next_sync_point));

			return 0;
			}
		}

	double pseudo_time = current_packet.ts - first_timestamp;
	double ct = (current_time(true) - first_wallclock) * pseudo_realtime;

	return pseudo_time <= ct ? bro_start_time + pseudo_time : 0;
	}

void PktSrc::Init()
	{
	Open();
	}

void PktSrc::Done()
	{
	Close();
	}

void PktSrc::GetFds(int* read, int* write, int* except)
	{
	if ( pseudo_realtime )
		{
		// Select would give erroneous results. But we simulate it
		// by setting idle accordingly.
		SetIdle(CheckPseudoTime() == 0);
		return;
		}

	if ( IsOpen() && props.selectable_fd >= 0 )
		*read = props.selectable_fd;
	}

double PktSrc::NextTimestamp(double* local_network_time)
	{
	if ( ! IsOpen() )
		return -1.0;

	if ( ! ExtractNextPacketInternal() )
		return -1.0;

	if ( pseudo_realtime )
		{
		// Delay packet if necessary.
		double packet_time = CheckPseudoTime();
		if ( packet_time )
			return packet_time;

		SetIdle(true);
		return -1.0;
		}

	return current_packet.ts;
	}

void PktSrc::Process()
	{
	if ( ! IsOpen() )
		return;

	if ( ! ExtractNextPacketInternal() )
		return;

	int pkt_hdr_size = props.hdr_size;

	// Unfortunately some packets on the link might have MPLS labels
	// while others don't. That means we need to ask the link-layer if
	// labels are in place.
	bool have_mpls = false;

	int protocol = 0;
	const u_char* data = current_packet.data;

	switch ( props.link_type ) {
	case DLT_NULL:
		{
		protocol = (data[3] << 24) + (data[2] << 16) + (data[1] << 8) + data[0];

		// From the Wireshark Wiki: "AF_INET6, unfortunately, has
		// different values in {NetBSD,OpenBSD,BSD/OS},
		// {FreeBSD,DragonFlyBSD}, and {Darwin/Mac OS X}, so an IPv6
		// packet might have a link-layer header with 24, 28, or 30
		// as the AF_ value." As we may be reading traces captured on
		// platforms other than what we're running on, we accept them
		// all here.
		if ( protocol != AF_INET
		     && protocol != AF_INET6
		     && protocol != 24
		     && protocol != 28
		     && protocol != 30 )
			{
			Weird("non_ip_packet_in_null_transport", &current_packet);
			data = 0;
			return;
			}

		break;
		}

	case DLT_EN10MB:
		{
		// Get protocol being carried from the ethernet frame.
		protocol = (data[12] << 8) + data[13];

		switch ( protocol )
			{
			// MPLS carried over the ethernet frame.
			case 0x8847:
				have_mpls = true;
				break;

			// VLAN carried over the ethernet frame.
			case 0x8100:
				data += GetLinkHeaderSize(props.link_type);
				data += 4; // Skip the vlan header
				pkt_hdr_size = 0;

				// Check for 802.1ah (Q-in-Q) containing IP.
				// Only do a second layer of vlan tag
				// stripping because there is no
				// specification that allows for deeper
				// nesting.
				if ( ((data[2] << 8) + data[3]) == 0x0800 )
					data += 4;

				break;

			// PPPoE carried over the ethernet frame.
			case 0x8864:
				data += GetLinkHeaderSize(props.link_type);
				protocol = (data[6] << 8) + data[7];
				data += 8; // Skip the PPPoE session and PPP header
				pkt_hdr_size = 0;

				if ( protocol != 0x0021 && protocol != 0x0057 )
					{
					// Neither IPv4 nor IPv6.
					Weird("non_ip_packet_in_pppoe_encapsulation", &current_packet);
					data = 0;
					return;
					}
				break;
			}

		break;
		}

	case DLT_PPP_SERIAL:
		{
		// Get PPP protocol.
		protocol = (data[2] << 8) + data[3];

		if ( protocol == 0x0281 )
			// MPLS Unicast
			have_mpls = true;

		else if ( protocol != 0x0021 && protocol != 0x0057 )
			{
			// Neither IPv4 nor IPv6.
			Weird("non_ip_packet_in_ppp_encapsulation", &current_packet);
			data = 0;
			return;
			}
		break;
		}
	}

	if ( have_mpls )
		{
		// Remove the data link layer
		data += GetLinkHeaderSize(props.link_type);

		// Denote a header size of zero before the IP header
		pkt_hdr_size = 0;

		// Skip the MPLS label stack.
		bool end_of_stack = false;

		while ( ! end_of_stack )
			{
			end_of_stack = *(data + 2) & 0x01;
			data += 4;
			}
		}

	if ( pseudo_realtime )
		{
		current_pseudo = CheckPseudoTime();
		net_packet_arrival(current_pseudo, current_packet.hdr, current_packet.data, pkt_hdr_size, this);
		if ( ! first_wallclock )
			first_wallclock = current_time(true);
		}

	else
		net_packet_arrival(current_packet.ts, current_packet.hdr, current_packet.data, pkt_hdr_size, this);

	have_packet = 0;
	DoneWithPacket(&current_packet);
	}

const char* PktSrc::Tag()
	{
	return "PktSrc";
	}

int PktSrc::ExtractNextPacketInternal()
	{
	if ( have_packet )
		return true;

	have_packet = false;

	// Don't return any packets if processing is suspended (except for the
	// very first packet which we need to set up times).
	if ( net_is_processing_suspended() && first_timestamp )
		{
		SetIdle(true);
		return 0;
		}

	if ( pseudo_realtime )
		current_wallclock = current_time(true);

	if ( ExtractNextPacket(&current_packet) )
		{
		if ( ! first_timestamp )
			first_timestamp = current_packet.ts;

		have_packet = true;
		return 1;
		}

	if ( pseudo_realtime && using_communication && ! IsOpen() )
		{
		// Source has gone dry, we're done.
		if ( remote_trace_sync_interval )
			remote_serializer->SendFinalSyncPoint();
		else
			remote_serializer->Terminate();
		}

	SetIdle(true);
	return 0;
	}

int PktSrc::PrecompileFilter(int index, const std::string& filter)
	{
	return 1;
	}

int PktSrc::SetFilter(int index)
	{
	return 1;
	}