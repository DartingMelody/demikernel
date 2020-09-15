use std::rc::Rc;
use std::num::Wrapping;
use crate::fail::Fail;
use super::super::state::ControlBlock;
use super::super::state::sender::SenderState;
use super::super::state::receiver::ReceiverState;
use std::future::Future;
use futures::{future, FutureExt};

async fn rx_ack_sender(cb: Rc<ControlBlock>) -> Result<!, Fail> {
    loop {
        let (receiver_st, receiver_st_changed) = cb.receiver.state.watch();
        if receiver_st == ReceiverState::Open || receiver_st == ReceiverState::AckdFin {
            receiver_st_changed.await;
            continue;
        }

        // Wait for all data to be acknowledged.
        let (ack_seq, ack_seq_changed) = cb.receiver.ack_seq_no.watch();
        let recv_seq = cb.receiver.recv_seq_no.get();

        if ack_seq != recv_seq {
            ack_seq_changed.await;
            continue;
        }

        // Send ACK segment
        let remote_link_addr = cb.arp.query(cb.remote.address()).await?;
        let segment = cb.tcp_segment().ack(recv_seq + Wrapping(1));
        cb.emit(segment, remote_link_addr);

        cb.receiver.state.set(ReceiverState::AckdFin);
    }
}

async fn tx_fin_sender(cb: Rc<ControlBlock>) -> Result<!, Fail> {
    loop {
        let (sender_st, sender_st_changed) = cb.sender.state.watch();
        match sender_st {
            SenderState::Open | SenderState::SentFin | SenderState::FinAckd => {
                sender_st_changed.await;
                continue;
            },
            SenderState::Closed => {
                // Wait for `sent_seq_no` to catch up to `unsent_seq_no` and
                // then send a FIN segment.
                let (sent_seq, sent_seq_changed) = cb.sender.sent_seq_no.watch();
                let unsent_seq = cb.sender.unsent_seq_no.get();

                if sent_seq != unsent_seq {
                    sent_seq_changed.await;
                    continue;
                }

                // TODO: When do we retransmit this?
                let remote_link_addr = cb.arp.query(cb.remote.address()).await?;
                let segment = cb.tcp_segment()
                    .seq_num(sent_seq + Wrapping(1))
                    .fin();
                cb.emit(segment, remote_link_addr);

                cb.sender.state.set(SenderState::SentFin);
            },
        }
    }
}

async fn close_wait(cb: Rc<ControlBlock>) -> Result<!, Fail> {
    loop {
        let (sender_st, sender_st_changed) = cb.sender.state.watch();
        if sender_st != SenderState::FinAckd {
            sender_st_changed.await;
            continue;
        }

        let (receiver_st, receiver_st_changed) = cb.receiver.state.watch();
        if receiver_st != ReceiverState::AckdFin {
            receiver_st_changed.await;
            continue;
        }

        // TODO: Wait for 2*MSL if active close.
        return Err(Fail::ConnectionAborted {});
    }
}

pub async fn closer(cb: Rc<ControlBlock>) -> Result<!, Fail> {
    futures::select_biased! {
        r = rx_ack_sender(cb.clone()).fuse() => r,
        r = tx_fin_sender(cb.clone()).fuse() => r,
        r = close_wait(cb).fuse() => r,
    }
}