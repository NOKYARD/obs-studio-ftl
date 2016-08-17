#include "ftl.h"

static void *recv_thread(void *data);

#define do_log(level, format, ...) \
	blog(level, "[rtmp stream: 'ftl'] " format, ##__VA_ARGS__)

#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

int FTL_init_data(ftl_t *ftl, char *ingest) {
	WSADATA wsa;
	int ret;
	struct hostent *server;
	
	//Initialise winsock
	printf("Initialising Winsock...");
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		printf("Failed. Error Code : %d", WSAGetLastError());
		return EXIT_FAILURE;
	}
	printf("Initialised\n");

	//Create a socket
	if ((ftl->data_sock = socket(AF_INET, SOCK_DGRAM, 0)) == INVALID_SOCKET)
	{
		printf("Could not create socket : %d", WSAGetLastError());
	}
	printf("Socket created.\n");

	if ( (server = gethostbyname(ingest)) == NULL ) {
		printf("ERROR, no such host as %s\n", ingest);
		return -1;
	}

	//Prepare the sockaddr_in structure
	ftl->server_addr.sin_family = AF_INET;
	memcpy((char *)&ftl->server_addr.sin_addr.s_addr, (char *)server->h_addr, server->h_length);
	ftl->server_addr.sin_port = htons(FTL_UDP_DATA_PORT);
/*
	if (bind(ftl->data_sock, (struct sockaddr *)&ftl->server_addr, sizeof(ftl->server_addr)) == SOCKET_ERROR)
	{
		printf("Bind failed with error code : %d", WSAGetLastError());
		exit(EXIT_FAILURE);
	}
	puts("Bind done");
	*/

	ret = pthread_create(&ftl->recv_thread, NULL, recv_thread, ftl);
	if (ret != 0) {
		//FTL_Close(ftl);
		//warn("Failed to create send thread");
		return -1;
	}

	ftl->max_mtu = 1392;
	if ((ftl->pktBuf = (uint8_t*)malloc(ftl->max_mtu)) == NULL) {
		warn("Failed to alloc memory for pkt buffer\n");
		return -1;
	}

	ftl->video_sn=0;
	ftl->audio_sn = 0;
	ftl->video_ptype = 96;
	ftl->video_timestamp = 0;
	ftl->video_timestamp_step = 90000 / 30;
	ftl->audio_timestamp = 0;
	ftl->audio_timestamp_step = 48000 / 50;
	ftl->video_ssrc = 0x12345678;

	return 0;
}

int FTL_set_audio_ssrc(ftl_t *ftl, uint32_t ssrc) {
	ftl->audio_ssrc = ssrc;

	return 0;
}

int FTL_set_video_ssrc(ftl_t *ftl, uint32_t ssrc) {
	ftl->video_ssrc = ssrc;

	return 0;
}

int FTL_set_audio_ptype(ftl_t *ftl, uint8_t p_type) {
	ftl->audio_ptype = p_type;

	return 0;
}

int FTL_set_video_ptype(ftl_t *ftl, uint8_t p_type) {
	ftl->video_ptype = p_type;

	return 0;
}

int _make_video_rtp_packet(ftl_t *ftl, uint8_t *in, int in_len, uint8_t *out, int *out_len, int first_pkt) {
	uint8_t sbit, ebit;
	int frag_len;

	sbit = first_pkt ? 1 : 0;
	ebit = (in_len + RTP_HEADER_BASE_LEN + RTP_FUA_HEADER_LEN) < ftl->max_mtu;

	uint32_t rtp_header;

	rtp_header = htonl((2 << 30) | (ftl->video_ptype << 16) | ftl->video_sn);
	*((uint32_t*)out)++ = rtp_header;
	rtp_header = htonl(ftl->video_timestamp);
	*((uint32_t*)out)++ = rtp_header;
	rtp_header = htonl(ftl->video_ssrc);
	*((uint32_t*)out)++ = rtp_header;

	ftl->video_sn++;

	if (sbit && ebit) {
		sbit = ebit = 0;
		frag_len = in_len;
		*out_len = frag_len + RTP_HEADER_BASE_LEN;
		memcpy(out, in, frag_len);
	} else {

		if (sbit) {
			ftl->current_nalu_type = in[0];
			in += 1;
			in_len--;
		}

		out[0] = ftl->current_nalu_type & 0xE0 | 28;
		out[1] = (sbit << 7) | (ebit << 6) | (ftl->current_nalu_type & 0x1F);

		out += 2;
		
		frag_len = ftl->max_mtu - RTP_HEADER_BASE_LEN - RTP_FUA_HEADER_LEN;

		if (frag_len > in_len) {
			frag_len = in_len;
		}

		memcpy(out, in, frag_len);

		*out_len = frag_len + RTP_HEADER_BASE_LEN + RTP_FUA_HEADER_LEN;
	}

	return frag_len + sbit;
}

int _make_audio_rtp_packet(ftl_t *ftl, uint8_t *in, int in_len, uint8_t *out, int *out_len) {
	int payload_len = in_len;

	uint32_t rtp_header;

	rtp_header = htonl((2 << 30) | (1 << 23) | (ftl->audio_ptype << 16) | ftl->audio_sn);
	*((uint32_t*)out)++ = rtp_header;
	rtp_header = htonl(ftl->audio_timestamp);
	*((uint32_t*)out)++ = rtp_header;
	rtp_header = htonl(ftl->audio_ssrc);
	*((uint32_t*)out)++ = rtp_header;

	ftl->audio_sn++;
	ftl->audio_timestamp += ftl->audio_timestamp_step;

	memcpy(out, in, payload_len);

	*out_len = payload_len + RTP_HEADER_BASE_LEN;

	return in_len;
}

int _set_marker_bit(ftl_t *ftl, uint8_t *in) {
	uint32_t rtp_header;

	rtp_header = ntohl(*((uint32_t*)in));
	rtp_header |= 1 << 23; /*set marker bit*/
	*((uint32_t*)in) = htonl(rtp_header);
	
	ftl->video_timestamp += ftl->video_timestamp_step;

	return 0;
}

int FTL_sendPackets(ftl_t *ftl, struct encoder_packet *packet, int idx, bool is_header){

	if (packet->type == OBS_ENCODER_VIDEO) {
		int consumed = 0;
		int len = packet->size;

		unsigned char *p = packet->data;

		while (consumed < packet->size) {
			if (is_header) {
				if (consumed == 0) {
					p += 6; //first 6 bytes are some obs header with part of the sps
					consumed += 6;
				} else {
					p += 1; //another spacer byte of 0x1
					consumed += 1;
				}

				len = p[0] << 8 | p[1];
				p += 2;
				consumed += 2;
			}
			else {
				len = p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];

				if (len > (packet->size - consumed)) {
					warn("ERROR: got len of %d but packet only has %d left\n", len, (packet->size - consumed));
				}

				consumed += 4;
				p += 4;
			}

			//info("Got Video Packet of type %d size %d (%02X %02X %02X %02X %02X %02X %02X %02X)\n", p[0] & 0x1F, len, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

			int pkt_len;
			int payload_size;

			int remaining = len;
			int first_fu = 1;

			while(remaining > 0) {

				payload_size = _make_video_rtp_packet(ftl, p, remaining, ftl->pktBuf, &pkt_len, first_fu);

				first_fu = 0;
				remaining -= payload_size;
				consumed += payload_size;
				p += payload_size;

				/*if all data has been consumed set marker bit*/
				if ((packet->size - consumed) == 0 && !is_header) {
					_set_marker_bit(ftl, ftl->pktBuf);
				}

				if (sendto(ftl->data_sock, ftl->pktBuf, pkt_len, 0, (struct sockaddr*) &ftl->server_addr, sizeof(ftl->server_addr)) == SOCKET_ERROR)
				{
					warn("sendto() failed with error code : %d", WSAGetLastError());
				}
			}
		}

	}
	else if(packet->type == OBS_ENCODER_AUDIO) {
		int pkt_len;
		_make_audio_rtp_packet(ftl, packet->data, packet->size, ftl->pktBuf, &pkt_len);

		if (sendto(ftl->data_sock, ftl->pktBuf, pkt_len, 0, (struct sockaddr*) &ftl->server_addr, sizeof(ftl->server_addr)) == SOCKET_ERROR)
		{
			warn("sendto() failed with error code : %d", WSAGetLastError());
		}
	}
	else {
		warn("Got packet type %d\n", packet->type);
	}

	return 0;
}

#if 0
ret = pthread_create(&ftl->recv_thread, NULL, send_thread, stream);
if (ret != 0) {
	RTMP_Close(&stream->rtmp);
	warn("Failed to create send thread");
	return OBS_OUTPUT_ERROR;
}

os_atomic_set_bool(&stream->active, true);
while (next) {
	if (!send_meta_data(stream, idx++, &next)) {
		warn("Disconnected while attempting to connect to "
			"server.");
		return OBS_OUTPUT_DISCONNECTED;
	}
}
obs_output_begin_data_capture(stream->output, 0);

return OBS_OUTPUT_SUCCESS;
#endif
int FTL_LogSetCallback() {
}

static void *recv_thread(void *data)
{
	ftl_t *ftl = (ftl_t *)data;
	int ret;
	unsigned char *buf;

	if ((buf = (unsigned char*)malloc(BUFLEN)) == NULL) {
		printf("Failed to allocate recv buffer\n");
		return NULL;
	}


	//os_set_thread_name("ftl-stream: recv_thread");

	while (1) {

#ifdef _WIN32
		ret = recv(ftl->data_sock, buf, BUFLEN, 0);
#else
		ret = recv(stream->sb_socket, buf, bytes, 0);
#endif
		if (ret <= 0) {
			continue;
		}

		info("Got recv of size %d bytes\n", ret);

		int version, padding, feedbackType, ptype, length, ssrcSender, ssrcMedia;
		uint16_t snBase, blp, sn;
		int recv_len = ret;

		if (recv_len < 2) {
			warn("recv packet too small to parse, discarding\n");
			continue;
		}

		/*extract rtp header*/
		version = (buf[0] >> 6) & 0x3;
		padding = (buf[0] >> 5) & 0x1;
		feedbackType = buf[0] & 0x1F;
		ptype = buf[1];

		if (feedbackType == 1 && ptype == 205) {
			info("Got NACK retransmit request\n");

			length = ntohs(*((uint16_t*)(buf + 2)));

			if (recv_len < ((length+1) * 4)) {
				warn("reported len was %d but packet is only %d...discarding\n", recv_len, ((length + 1) * 4));
				continue;
			}

			ssrcSender = ntohl(*((uint32_t*)(buf+4)));
			ssrcMedia = ntohl(*((uint32_t*)(buf+8)));

			uint16_t *p = (uint16_t *)(buf + 12);

			for (int fci = 0; fci < (length - 2); fci++) {
				//request the first sequence number
				snBase = ntohs(*p++);
				info("[%d] nack request for sn %d\n", ssrcMedia, snBase);
				blp = ntohs(*p++);
				if (blp) {
					for (int i = 0; i < 16; i++) {
						if ((blp & (1 << i)) != 0) {
							sn = snBase + i + 1;
							info("[%d] nack request for sn %d\n", ssrcMedia, sn);
						}
					}
				}
			}
		}


	}


#if 0

	while (os_sem_wait(stream->send_sem) == 0) {
		struct encoder_packet packet;

		if (stopping(stream) && stream->stop_ts == 0) {
			break;
		}

		if (!get_next_packet(stream, &packet))
			continue;

		if (stopping(stream)) {
			if (packet.sys_dts_usec >= (int64_t)stream->stop_ts) {
				obs_free_encoder_packet(&packet);
				break;
			}
		}

		/*
		if (!stream->sent_headers) {
		if (!send_headers(stream)) {
		os_atomic_set_bool(&stream->disconnected, true);
		break;
		}
		}
		*/

		if (send_packet(stream, &packet, false, packet.track_idx) < 0) {
			os_atomic_set_bool(&stream->disconnected, true);
			break;
		}
	}

	if (disconnected(stream)) {
		info("Disconnected from %s", stream->path.array);
	}
	else {
		info("User stopped the stream");
	}

	//RTMP_Close(&stream->rtmp);

	if (!stopping(stream)) {
		pthread_detach(stream->send_thread);
		obs_output_signal_stop(stream->output, OBS_OUTPUT_DISCONNECTED);
	}
	else {
		obs_output_end_data_capture(stream->output);
	}

	free_packets(stream);
	os_event_reset(stream->stop_event);
	os_atomic_set_bool(&stream->active, false);
	stream->sent_headers = false;
	return NULL;

#endif
}