# target : main.o sub.o
#          g++ main.o sub.o -o exfile
# main.o : main.cpp
#          g++ -c main.cpp
# sub.o : sub.cpp
#          g++ -c sub.cpp

CC  = g++
CFLAGS  =-std=c++17 -D__RTP_NO_CRYPTO__ -DUVGRTP_HAVE_SENDMSG=1
TARGET  = Sample
SRCS    = receiving_hook.cpp
OBJS    = receiving_hook.o
UVGRTPINCDIR = -I./
J2KPARSERINCDIR = -I./

UVGRTPOBJS = clock.o context.o crypto.o frame.o frame_queue.o holepuncher.o hostname.o media_stream.o poll.o random.o reception_flow.o rtcp.o rtcp_packets.o rtcp_reader.o rtp.o session.o socket.o socketfactory.o version.o zrtp.o h26x.o h264.o h265.o h266.o media.o v3c.o base.o srtcp.o srtp.o commit.o confack.o confirm.o dh_kxchng.o error.o hello.o hello_ack.o zrtp_message.o zrtp_receiver.o

J2KPARSEROBJS =  j2k_header.o j2k_packet.o j2k_tile.o utils.o

INCDIR  = -I.

LIBDIR  =

LIBS    = -lpthread

$(TARGET): $(OBJS) $(UVGRTPOBJS) $(J2KPARSEROBJS)
	$(CC) -o $@ $^ $(LIBDIR) $(LIBS)

$(OBJS): $(SRCS)
	$(CC) $(CFLAGS) $(INCDIR) -c $(SRCS)

clock.o : uvgrtp/clock.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/clock.cc
context.o : uvgrtp/context.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/context.cc
crypto.o : uvgrtp/crypto.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/crypto.cc
frame.o : uvgrtp/frame.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/frame.cc
frame_queue.o : uvgrtp/frame_queue.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/frame_queue.cc
holepuncher.o : uvgrtp/holepuncher.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/holepuncher.cc
hostname.o : uvgrtp/hostname.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/hostname.cc
media_stream.o : uvgrtp/media_stream.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/media_stream.cc
poll.o : uvgrtp/poll.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/poll.cc
random.o : uvgrtp/random.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/random.cc
reception_flow.o : uvgrtp/reception_flow.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/reception_flow.cc
rtcp.o : uvgrtp/rtcp.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/rtcp.cc
rtcp_packets.o : uvgrtp/rtcp_packets.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/rtcp_packets.cc
rtcp_reader.o : uvgrtp/rtcp_reader.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/rtcp_reader.cc
rtp.o : uvgrtp/rtp.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/rtp.cc
session.o : uvgrtp/session.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/session.cc
socket.o : uvgrtp/socket.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/socket.cc
socketfactory.o : uvgrtp/socketfactory.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/socketfactory.cc
version.o : uvgrtp/version.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/version.cc
zrtp.o : uvgrtp/zrtp.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/zrtp.cc
h26x.o : uvgrtp/formats/h26x.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/formats/h26x.cc
h264.o : uvgrtp/formats/h264.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/formats/h264.cc
h265.o : uvgrtp/formats/h265.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/formats/h265.cc
h266.o : uvgrtp/formats/h266.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/formats/h266.cc
media.o : uvgrtp/formats/media.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/formats/media.cc
v3c.o : uvgrtp/formats/v3c.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/formats/v3c.cc
base.o : uvgrtp/srtp/base.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/srtp/base.cc
srtcp.o : uvgrtp/srtp/srtcp.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/srtp/srtcp.cc
srtp.o : uvgrtp/srtp/srtp.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/srtp/srtp.cc
commit.o : uvgrtp/zrtp/commit.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/zrtp/commit.cc
confack.o : uvgrtp/zrtp/confack.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/zrtp/confack.cc
confirm.o : uvgrtp/zrtp/confirm.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/zrtp/confirm.cc
dh_kxchng.o : uvgrtp/zrtp/dh_kxchng.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/zrtp/dh_kxchng.cc
error.o : uvgrtp/zrtp/error.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/zrtp/error.cc
hello.o : uvgrtp/zrtp/hello.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/zrtp/hello.cc
hello_ack.o : uvgrtp/zrtp/hello_ack.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/zrtp/hello_ack.cc
zrtp_message.o : uvgrtp/zrtp/zrtp_message.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/zrtp/zrtp_message.cc
zrtp_receiver.o : uvgrtp/zrtp/zrtp_receiver.cc
	$(CC) $(CFLAGS) $(UVGRTPINCDIR) -c uvgrtp/zrtp/zrtp_receiver.cc

j2k_header.o : packet_parser/j2k_header.cpp
	$(CC) $(CFLAGS) $(J2KPARSERINCDIR) -c packet_parser/j2k_header.cpp
j2k_packet.o : packet_parser/j2k_packet.cpp
	$(CC) $(CFLAGS) $(J2KPARSERINCDIR) -c packet_parser/j2k_packet.cpp
j2k_tile.o : packet_parser/j2k_tile.cpp
	$(CC) $(CFLAGS) $(J2KPARSERINCDIR) -c packet_parser/j2k_tile.cpp
utils.o : packet_parser/utils.cpp
	$(CC) $(CFLAGS) $(J2KPARSERINCDIR) -c packet_parser/utils.cpp

all: clean $(OBJS) $(UVGRTPOBJS) $(J2KPARSEROBJS) $(TARGET)
clean:
	-rm -f $(OBJS) $(UVGRTPOBJS) $(J2KPARSEROBJS) $(TARGET) *.d