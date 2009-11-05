/*
 *  datasendrecv.cpp
 *  serp++
 *
 *  Created by Victor Grishchenko on 3/6/09.
 *  Copyright 2009 Delft University of Technology. All rights reserved.
 *
 */
#include <algorithm>
#include <glog/logging.h>
#include "p2tp.h"


using namespace p2tp;
using namespace std; // FIXME remove

/*
 TODO  25 Oct 18:55
 - move hint_out_, piece picking to piece picker (needed e.g. for the case of channel drop)
 - ANY_LAYER
 - range: ALL
 - randomized testing of advanced ops (new testcase)
 - PeerCwnd()
 - bins hint_out_, tbqueue hint_out_ts_
 
 */

void	Channel::AddPeakHashes (Datagram& dgram) {
	for(int i=0; i<file().peak_count(); i++) {
        bin64_t peak = file().peak(i);
		dgram.Push8(P2TP_HASH);
		dgram.Push32((uint32_t)peak);
		dgram.PushHash(file().peak_hash(i));
        //DLOG(INFO)<<"#"<<id<<" +pHASH"<<file().peak(i);
        dprintf("%s #%i +phash (%i,%lli)\n",Datagram::TimeStr(),id,peak.layer(),peak.offset());
	}
}


void	Channel::AddUncleHashes (Datagram& dgram, bin64_t pos) {
    bin64_t peak = file().peak_for(pos);
    while (pos!=peak && ack_in_.get(pos.parent())==bins::EMPTY) {
        bin64_t uncle = pos.sibling();
		dgram.Push8(P2TP_HASH);
		dgram.Push32((uint32_t)uncle);
		dgram.PushHash( file().hash(uncle) );
        //DLOG(INFO)<<"#"<<id<<" +uHASH"<<uncle;
        dprintf("%s #%i +hash (%i,%lli)\n",Datagram::TimeStr(),id,uncle.layer(),uncle.offset());
        pos = pos.parent();
    }
}


bin64_t		Channel::DequeueHint () { // TODO: resilience
    bin64_t send = bin64_t::NONE;
    while (!hint_in_.empty() && send==bin64_t::NONE) {
        bin64_t hint = hint_in_.front();
        hint_in_.pop_front();
        send = file().ack_out().find_filtered
            (ack_in_,hint,0,bins::FILLED);
        dprintf("%s #%i may_send %lli\n",Datagram::TimeStr(),id,send.base_offset());
        if (send!=bin64_t::NONE)
            while (send!=hint) {
                hint = hint.towards(send);
                hint_in_.push_front(hint.sibling());
            }
    }
    return send;
}


/*void	Channel::CleanStaleHints () {
	while ( !hint_out.empty() && file().ack_out().get(hint_out.front().bin)==bins::FILLED ) 
		hint_out.pop_front();  // FIXME must normally clear fulfilled entries
	tint timed_out = Datagram::now - cc_->RoundTripTime()*8;
	while ( !hint_out.empty() && hint_out.front().time < timed_out ) {
        file().picker()->Snubbed(hint_out.front().bin);
		hint_out.pop_front();
	}
}*/


void	Channel::AddHandshake (Datagram& dgram) {
	if (!peer_channel_id_) { // initiating
		dgram.Push8(P2TP_HASH);
		dgram.Push32(bin64_t::ALL32);
		dgram.PushHash(file().root_hash());
        dprintf("%s #%i +hash ALL %s\n",
                Datagram::TimeStr(),id,file().root_hash().hex().c_str());
	}
	dgram.Push8(P2TP_HANDSHAKE);
	dgram.Push32(EncodeID(id));
    dprintf("%s #%i +hs\n",Datagram::TimeStr(),id);
    AddAck(dgram);
    ack_out_.clear();
}


void    Channel::ClearStaleDataOut() {
    int oldsize = data_out_.size();
    while ( data_out_.size() && data_out_.front().time < 
           Datagram::now - rtt_avg_ - dev_avg_*4 )
        data_out_.pop_front();
    if (data_out_.size()!=oldsize)
        cc_->OnAckRcvd(bin64_t::NONE);
}


void	Channel::Send () {
    Datagram dgram(socket_,peer());
    dgram.Push32(peer_channel_id_);
    bin64_t data = bin64_t::NONE;
    if ( is_established() ) {
        AddAck(dgram);
        AddHint(dgram);
        ClearStaleDataOut();
        if (cc_->MaySendData()) 
            data = AddData(dgram);
        else
            dprintf("%s #%i no cwnd\n",Datagram::TimeStr(),id);
    } else {
        AddHandshake(dgram);
        AddAck(dgram);
    }
    dprintf("%s #%i sent %ib %s\n",Datagram::TimeStr(),id,dgram.size(),peer().str().c_str());
	PCHECK( dgram.Send() != -1 )<<"error sending";
    if (dgram.size()==4) // only the channel id; bare keep-alive
        data = bin64_t::ALL;
    cc_->OnDataSent(data);
    last_send_time_ = Datagram::now;
    RequeueSend(cc_->NextSendTime());
}


void	Channel::AddHint (Datagram& dgram) {

    while (!hint_out_.empty() &&
            (hint_out_.front().time<Datagram::now-TINT_SEC ||
            file().ack_out().get(hint_out_.front().bin)==bins::FILLED ) ) {
        file().picker().Expired(hint_out_.front().bin);
        hint_out_.pop_front();
    }
    uint64_t hinted = 0;
    for(tbqueue::iterator i=hint_out_.begin(); i!=hint_out_.end(); i++)
        hinted += i->bin.width();
    int bps = PeerBPS();
    dprintf("%s #%i hinted %lli peer_bps %i\n",Datagram::TimeStr(),id,hinted,bps);
    //float peer_cwnd = cc_->PeerBPS() * cc_->RoundTripTime() / TINT_SEC;
    
    if ( bps > hinted*1024 ) { //hinted*1024 < peer_cwnd*4 ) {
        
        uint8_t layer = 2; // actually, enough
        bin64_t hint = file().picker().Pick(ack_in_,layer);
        // FIXME FIXME FIXME: any layer
        if (hint==bin64_t::NONE)
            hint = file().picker().Pick(ack_in_,0);
        
        if (hint!=bin64_t::NONE) {
            hint_out_.push_back(hint);
            dgram.Push8(P2TP_HINT);
            dgram.Push32(hint);
            dprintf("%s #%i +hint (%i,%lli)\n",Datagram::TimeStr(),id,hint.layer(),hint.offset());
        }
        
    }
}


bin64_t		Channel::AddData (Datagram& dgram) {
	if (!file().size()) // know nothing
		return bin64_t::NONE;
	bin64_t tosend = DequeueHint();
    if (tosend==bin64_t::NONE) 
        return bin64_t::NONE;
    if (ack_in_.is_empty() && file().size())
        AddPeakHashes(dgram);
    AddUncleHashes(dgram,tosend);
    uint8_t buf[1024];
    size_t r = pread(file().file_descriptor(),buf,1024,tosend.base_offset()<<10); 
    // TODO: ??? corrupted data, retries
    if (r<0) {
        PLOG(ERROR)<<"error on reading";
        return 0;
    }
    assert(dgram.space()>=r+4+1);
    dgram.Push8(P2TP_DATA);
    dgram.Push32(tosend);
    dgram.Push(buf,r);
    dprintf("%s #%i +data (%lli)\n",Datagram::TimeStr(),id,tosend.base_offset());
    data_out_.push_back(tosend);
	return tosend;
}


void	Channel::AddTs (Datagram& dgram) {
    dgram.Push8(P2TP_TS);
    dgram.Push64(data_in_.time);
    dprintf("%s #%i +ts %lli\n",Datagram::TimeStr(),id,data_in_.time);
}


void	Channel::AddAck (Datagram& dgram) {
	if (data_in_.bin!=bin64_t::NONE) {
        AddTs(dgram);
        bin64_t pos = data_in_.bin;
		dgram.Push8(P2TP_ACK);
		dgram.Push32(pos);
		//dgram.Push64(data_in_.time);
        ack_out_.set(pos);
        dprintf("%s #%i +ack (%i,%lli) %s\n",Datagram::TimeStr(),id,
                pos.layer(),pos.offset(),Datagram::TimeStr(data_in_.time));
        data_in_ = tintbin(0,bin64_t::NONE);
	}
    for(int count=0; count<4; count++) {
        bin64_t ack = file().ack_out().find_filtered(ack_out_, bin64_t::ALL, 0, bins::FILLED);
        // TODO bins::ANY_LAYER
        if (ack==bin64_t::NONE)
            break;
        while (file().ack_out().get(ack.parent())==bins::FILLED)
            ack = ack.parent();
        ack_out_.set(ack);
        dgram.Push8(P2TP_ACK);
        dgram.Push32(ack);
        dprintf("%s #%i +ack (%i,%lli)\n",Datagram::TimeStr(),id,ack.layer(),ack.offset());
    }
}


void	Channel::Recv (Datagram& dgram) {
    bin64_t data = dgram.size() ? bin64_t::NONE : bin64_t::ALL;
	while (dgram.size()) {
		uint8_t type = dgram.Pull8();
		switch (type) {
            case P2TP_HANDSHAKE: OnHandshake(dgram); break;
			case P2TP_DATA:		data=OnData(dgram); break;
			case P2TP_TS:       OnTs(dgram); break;
			case P2TP_ACK:		OnAck(dgram); break;
			case P2TP_HASH:		OnHash(dgram); break;
			case P2TP_HINT:		OnHint(dgram); break;
            case P2TP_PEX_ADD:  OnPex(dgram); break;
			default:
				//LOG(ERROR) << this->id_string() << " malformed datagram";
				return;
		}
	}
    cc_->OnDataRecvd(data);
    last_recv_time_ = Datagram::now;
    if (data!=bin64_t::ALL)
        RequeueSend(Datagram::now);
}


void	Channel::OnHash (Datagram& dgram) {
	bin64_t pos = dgram.Pull32();
	Sha1Hash hash = dgram.PullHash();
	file().OfferHash(pos,hash);
    //DLOG(INFO)<<"#"<<id<<" .HASH"<<(int)pos;
    dprintf("%s #%i -hash (%i,%lli)\n",Datagram::TimeStr(),id,pos.layer(),pos.offset());
}


bin64_t Channel::OnData (Datagram& dgram) {
	bin64_t pos = dgram.Pull32();
    uint8_t *data;
    int length = dgram.Pull(&data,1024);
    bool ok = file().OfferData(pos, data, length) ;
    dprintf("%s #%i %cdata (%lli)\n",Datagram::TimeStr(),id,ok?'-':'!',pos.offset());
    if (ok) {
        data_in_ = tintbin(Datagram::now,pos);
        if (last_recv_time_) {
            tint dip = Datagram::now - last_recv_time_;
            dip_avg_ = ( dip_avg_*3 + dip ) >> 2;
        }
        return pos;
    } else
        return bin64_t::NONE;
}


void	Channel::OnAck (Datagram& dgram) {
	bin64_t ackd_pos = dgram.Pull32();
    if (ackd_pos.base_offset()>file().size())
        return;
    dprintf("%s #%i -ack (%i,%lli)\n",Datagram::TimeStr(),id,ackd_pos.layer(),ackd_pos.offset());
    for (int i=0; i<8 && i<data_out_.size(); i++) 
        if (data_out_[i].bin.within(ackd_pos)) {
            tintbin x = data_out_[i];
            data_out_[i].bin = bin64_t::ALL;
            tint rtt = Datagram::now-x.time;
            rtt_avg_ = (rtt_avg_*3 + rtt) >> 2;
            dev_avg_ = ( dev_avg_*3 + abs(rtt-rtt_avg_) ) >> 2;
            dprintf("%s #%i rtt %lli dev %lli\n",
                    Datagram::TimeStr(),id,rtt_avg_,dev_avg_);
            cc_->OnAckRcvd(x.bin);
        }
    while (data_out_.size() && data_out_.front().bin==bin64_t::ALL)
        data_out_.pop_front();
	ack_in_.set(ackd_pos);
}


/*void	Channel::OnAckTs (Datagram& dgram) {  // FIXME:   OnTs
	bin64_t pos = dgram.Pull32();
    tint ts = dgram.Pull64();
    // TODO sanity check
    dprintf("%s #%i -ackts (%i,%lli) %s\n",
            Datagram::TimeStr(),id,pos.layer(),pos.offset(),Datagram::TimeStr(ts));
	ack_in_.set(pos);
	cc_->OnAckRcvd(pos,ts);
}*/

void Channel::OnTs (Datagram& dgram) {
    peer_send_time_ = dgram.Pull64();
    dprintf("%s #%i -ts %lli\n",Datagram::TimeStr(),id,peer_send_time_);
}


void	Channel::OnHint (Datagram& dgram) {
	bin64_t hint = dgram.Pull32();
	hint_in_.push_back(hint);
    //RequeueSend(cc_->OnHintRecvd(hint));
    dprintf("%s #%i -hint (%i,%lli)\n",Datagram::TimeStr(),id,hint.layer(),hint.offset());
}


void Channel::OnHandshake (Datagram& dgram) {
    peer_channel_id_ = dgram.Pull32();
    dprintf("%s #%i -hs %i\n",Datagram::TimeStr(),id,peer_channel_id_);
    // FUTURE: channel forking
}


void Channel::OnPex (Datagram& dgram) {
    uint32_t ipv4 = dgram.Pull32();
    uint16_t port = dgram.Pull16();
    Address addr(ipv4,port);
    //if (peer_selector)
    //    peer_selector->AddPeer(Datagram::Address(addr,port),file().root_hash());
    file_->pex_in_.push_back(addr);
    if (file_->pex_in_.size()>1000)
        file_->pex_in_.pop_front(); 
    static int ENOUGH_PEERS_THRESHOLD = 20;
    if (file_->channel_count()<ENOUGH_PEERS_THRESHOLD) {
        int i = 0, chno;
        while ( (chno=file_->RevealChannel(i)) != -1 ) {
            if (channels[i]->peer()==addr) 
                break;
        }
        if (chno==-1)
            new Channel(file_,socket_,addr);
    }
}


void    Channel::AddPex (Datagram& dgram) {
    int ch = file().RevealChannel(this->pex_out_);
    if (ch==-1)
        return;
    Address a = channels[ch]->peer();
    dgram.Push8(P2TP_PEX_ADD);
    dgram.Push32(a.ipv4());
    dgram.Push16(a.port());
}


void	Channel::Recv (int socket) {
	Datagram data(socket);
	data.Recv();
	if (data.size()<4) 
		RETLOG("datagram shorter than 4 bytes");
	uint32_t mych = data.Pull32();
	Sha1Hash hash;
	Channel* channel;
	if (!mych) { // handshake initiated
		if (data.size()<1+4+1+4+Sha1Hash::SIZE) 
			RETLOG ("incorrect size initial handshake packet");
		uint8_t hashid = data.Pull8();
		if (hashid!=P2TP_HASH) 
			RETLOG ("no hash in the initial handshake");
		bin pos = data.Pull32();
		if (pos!=bin64_t::ALL32) 
			RETLOG ("that is not the root hash");
		hash = data.PullHash();
		FileTransfer* file = FileTransfer::Find(hash);
		if (!file) 
			RETLOG ("hash unknown, no such file");
        dprintf("%s #0 -hash ALL %s\n",Datagram::TimeStr(),hash.hex().c_str());
		channel = new Channel(file, socket, data.address());
	} else {
		mych = DecodeID(mych);
		if (mych>=channels.size()) {
            eprintf("invalid channel #%i\n",mych);
            return;
        }
		channel = channels[mych];
		if (!channel) 
			RETLOG ("channel is closed");
		if (channel->peer() != data.address()) 
			RETLOG ("invalid peer address");
        channel->own_id_mentioned_ = true;
	}
    channel->Recv(data);
}


bool tblater (const tintbin& a, const tintbin& b) {
    return a.time > b.time;
}


void    Channel::RequeueSend (tint next_time) {
    if (next_time==next_send_time_)
        return;
    next_send_time_ = next_time;
    send_queue.push_back(tintbin(next_time,id));
    push_heap(send_queue.begin(),send_queue.end(),tblater);
    dprintf("%s requeue #%i for %s\n",Datagram::TimeStr(),id,Datagram::TimeStr(next_time));
}


void    Channel::Loop (tint howlong) {  
	
    tint limit = Datagram::Time() + howlong;
    
    do {

        tint send_time(TINT_NEVER);
        Channel* sender(NULL);
        while (!send_queue.empty()) {
            send_time = send_queue.front().time;
            sender = channel((int)send_queue.front().bin);
            if (sender && sender->next_send_time_==send_time)
                break;
            sender = NULL; // it was a stale entry
            pop_heap(send_queue.begin(), send_queue.end(), tblater);
            send_queue.pop_back();
        }
        if (send_time>limit)
            send_time = limit;
        if (sender && send_time<=Datagram::now) {
            dprintf("%s #%i sch_send %s\n",Datagram::TimeStr(),sender->id,
                    Datagram::TimeStr(send_time));
            sender->Send();
            pop_heap(send_queue.begin(), send_queue.end(), tblater);
            send_queue.pop_back();
        } else {
            tint towait = send_time - Datagram::now;
            dprintf("%s waiting %lliusec\n",Datagram::TimeStr(),towait);
            int rd = Datagram::Wait(socket_count,sockets,towait);
            if (rd!=-1)
                Recv(rd);
        }
        
    } while (Datagram::Time()<limit);
    	
}



/*
 
 tint untiltime = Datagram::Time()+time;
 if (send_queue.empty())
 dprintf("%s empty send_queue\n", Datagram::TimeStr());
 
 while ( Datagram::now <= untiltime && !send_queue.empty() ) {
 
 // BUG BUG BUG  no scheduled sends => just listen
 
 tintbin next_send = send_queue.front();
 tint wake_on = next_send.time;
 Channel* sender = channel(next_send.bin);
 
 // BUG BUG BUG filter stale timeouts here
 
 //if (wake_on<=untiltime) {
 pop_heap(send_queue.begin(), send_queue.end(), tblater);
 send_queue.pop_back();
 //}// else
 //sender = 0; // BUG will never wake up
 
 if (sender->next_send_time_!=next_send.time)
 continue;
 
 if (wake_on<Datagram::now)
 wake_on = Datagram::now;
 if (wake_on>untiltime)
 wake_on = untiltime;
 tint towait = min(wake_on-Datagram::now,TINT_SEC);
 dprintf("%s waiting %lliusec\n",Datagram::TimeStr(),towait);
 int rd = Datagram::Wait(socket_count,sockets,towait);
 if (rd!=-1)
 Recv(rd);
 // BUG WRONG BUG WRONG  another may need to send
 if (sender) {
 dprintf("%s #%i sch_send %s\n",Datagram::TimeStr(),sender->id,
 Datagram::TimeStr(next_send.time));
 sender->Send();
 // if (sender->cc_->next_send_time==TINT_NEVER) 
 }
 
 }
 
 */
