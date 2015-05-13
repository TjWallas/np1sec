/**
 * Multiparty Off-the-Record Messaging library
 * Copyright (C) 2014, eQualit.ie
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of version 3 of the GNU Lesser General
 * Public License as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <assert.h>
#include <stdlib.h>
#include <string>

#include "src/session.h"
#include "src/exceptions.h"
#include "src/userstate.h"

using namespace std;

void MessageDigest::update(std::string new_message) {
  UNUSED(new_message);
  return;
}

void cb_send_heartbeat(void *arg) {
  np1secSession* session = (static_cast<np1secSession*>(arg));
  session->send("", session->forward_secrecy_load_type());
  session->restart_heartbeat_timer();
}
                         
void cb_ack_not_received(void *arg) {
  // Construct message for ack
  AckTimerOps* ack_timer_ops = static_cast<AckTimerOps*>(arg);

  std::string ack_failure_message = ack_timer_ops->participant->id.nickname + " failed to ack";
  ack_timer_ops->session->us->ops->display_message(ack_timer_ops->session->room_name, "np1sec directive", ack_failure_message, ack_timer_ops->session->us);
  delete ack_timer_ops;
}

void cb_send_ack(void *arg) {
  // Construct message with p.id
  AckTimerOps* ack_timer_ops = static_cast<AckTimerOps*>(arg);
  ack_timer_ops->session->send_ack_timer = nullptr;
  ack_timer_ops->session->send("", ack_timer_ops->session->forward_secrecy_load_type());

}

/**
 * The timer set upon of sending a message.
 * when this timer is timed out means that 
 * we haven't received our own message
 */
void cb_ack_not_sent(void* arg) {
  // Construct message with p.id
  AckTimerOps* ack_timer_ops = static_cast<AckTimerOps*>(arg);
  std::string ack_failure_message = "we did not receive our own sent message";
  ack_timer_ops->session->us->ops->display_message(ack_timer_ops->session->room_name, "np1sec directive", ack_failure_message, ack_timer_ops->session->us);

}

/**
 * when times out, the leaving user check 
 * all user's consistency before leaving
 */
void cb_leave(void *arg) {
  np1secSession* session = (static_cast<np1secSession*>(arg));

  session->check_leave_transcript_consistency();
  session->my_state = np1secSession::DEAD;

}

// TODO: Who is calling this? Answer: compute_session_id
// TODO: This should move to crypto really and called hash with
// overloaded parameters
// gcry_error_t np1secSession::compute_hash(HashBlock transcript_chain,
//                                      std::string message) {
//   //this just seems crazy to me
//   // assert(message.size() % 2 == 0);

//   // unsigned char *bin;
//   // const char *p = message.c_str();
//   // for (int i=0; i < message.size(); i++, p+=2) {
//   //   sscanf(p, "%2hhx", &bin);
//   // }
  
//   return cryptic.hash(bin, message.size()/2, transcript_chain, true);

// }

//All constructorsg
// np1secSession::np1secSession(np1secUserState *us)
//   :myself(us->user_nick())
// {
//   throw std::invalid_argument("Default constructor should not be used.");
// }

/**
   sole joiner constructor
 */
np1secSession::np1secSession(np1secUserState *us, std::string room_name,
                             Cryptic* current_ephemeral_crypto,
                             const UnauthenticatedParticipantList& sole_participant_view) : us(us), room_name(room_name), cryptic(*current_ephemeral_crypto), myself(*us->myself), heartbeat_timer(nullptr)

{
  engrave_transition_graph();

  populate_participants_and_peers(sole_participant_view);

  //if participant[myself].ephemeral is not crytpic ephemeral, halt
  compute_session_id();

  if (send_view_auth_and_share())
    my_state = REPLIED_TO_NEW_JOIN;
  else
    my_state = DEAD;

}

/**
 * This constructor should be only called when the session is generated
 * to join. That's why all participant are not authenticated.
 */
np1secSession::np1secSession(np1secUserState *us, std::string room_name,
                             Cryptic* current_ephemeral_crypto,
                             np1secMessage participants_info_message) :
  us(us), room_name(room_name),
  cryptic(*current_ephemeral_crypto),
  myself(*us->myself),
  heartbeat_timer(nullptr)
{
  engrave_transition_graph();

  //TODO:: obviously we need an access function to make sure there is joiner info
  //update participant info or let it be there if they are consistent with
  //UnauthenticatedParticipantList session_view() = participants_info_message.session_view();
  //the participant is added unauthenticated
  // populate_participants_and_peers(participants_info_message.session_view);

  // //TODO::make sure we are added correctly
  // //if participant[myself].ephemeral is not crytpic ephemeral, halt
  // compute_session_id();

  my_state = JOIN_REQUESTED; //because the join is requested that this message is
  //received
  StateAndAction auth_result = auth_and_reshare(participants_info_message);
  my_state = auth_result.first;
  
}

/**
 * Almost copy constructor, we only alter the plist
 */
// np1secSession::np1secSession(np1secSession& breeding_session, 
//               ParticipantMap updated_participants)
//   :participants(updated_participants)
// {

//   compute_session_id();
  
// }


/**
   Constructor being called by current participant receiving join request
   That's why (in room) participants are are already authenticated
   
     - in new session constructor these will happen
       - computes session_id
       - compute kc = kc_{sender, joiner}
       - compute z_sender (self)
       - set new session status to REPLIED_TO_NEW_JOIN
       - send 

  Or
  Leave Constructor being called by current participant receiving leave request
   
     - in new session constructor these will happen
       - drop leaver
       - computes session_id
       - compute z_sender (self)
       - set new session status to RE_SHARED

*/
np1secSession::np1secSession(np1secUserState *us,
                             std::string room_name,
                             Cryptic* current_ephemeral_crypto,
                             np1secMessage join_leave_message,
                             ParticipantMap current_authed_participants)
  :us(us), room_name(room_name),
   cryptic(*current_ephemeral_crypto),
   myself(*us->myself),
   participants(current_authed_participants),
   heartbeat_timer(nullptr)
{
   //TODO: not sure the session needs to know the room name: It needs because message class
  //need  to know to send the message to :-/
  //send should be the function of np1secRoom maybe :-?
  
  engrave_transition_graph();

  if (join_leave_message.message_type == np1secMessage::JOIN_REQUEST) {
    UnauthenticatedParticipant  joiner(join_leave_message.joiner_info);
    //TODO:: obviously we need an access function to make sure there is joiner info
    //update participant info or let it be there if they are consistent with

    this->participants.insert(pair<string,Participant> (joiner.participant_id.nickname, Participant(joiner, &cryptic)));

    //we have the ephemeral public key we can compute the p2p key
    populate_peers_from_participants();

    compute_session_id();

    if (session_id.get()) {
      send_view_auth_and_share(joiner.participant_id.nickname);
      my_state = REPLIED_TO_NEW_JOIN;
    }
  } else if ((join_leave_message.message_type == np1secMessage::IN_SESSION_MESSAGE) &&
             (join_leave_message.message_sub_type == np1secMessage::LEAVE_MESSAGE)) {
    string leaver_id = join_leave_message.sender_nick;
    assert(participants.find(leaver_id) != participants.end());
    participants.erase(leaver_id);

    populate_peers_from_participants();

    /*if (!participants[participant_id].set_ephemeral_key(it->ephemeral_key))
      throw np1secMessage::MessageFormatException;*/

    compute_session_id();
    if (send_new_share_message()) //TODO a reshare message
      my_state = RE_SHARED;
  }
  else {
    assert(0); //we shouldn't have been here
  }
}

/**
   Constructor being called by operator+ and operator- to breed 
   new (unestablished) session
   
     - in new session constructor these will happen
       - computes session_id
       - compute z_sender (self)
       - set new session status to RE_SHARED

*/
np1secSession::np1secSession(np1secUserState* us, std::string room_name,
                             Cryptic* current_ephemeral_crypto,
                             const ParticipantMap& current_authed_participants,
                             bool broadcast_participant_info)
  :us(us), room_name(room_name),
   cryptic(*current_ephemeral_crypto),
   myself(*us->myself),
  heartbeat_timer(nullptr)
   //TODO: not sure the session needs to know the room name
{
  engrave_transition_graph();

  participants = current_authed_participants;

  populate_peers_from_participants();
  compute_session_id();

  bool broadcast_result;
  if (broadcast_participant_info) {
    broadcast_result = send_view_auth_and_share();
  } else {
    broadcast_result = send_new_share_message();
  }

  if (broadcast_result)
    my_state = RE_SHARED;

}

np1secSession np1secSession::operator+(np1secSession a) {
  std::map<std::string, Participant> combination;
  combination.insert(participants.begin(), participants.end());
  combination.insert(a.participants.begin(), a.participants.end());
  np1secSession new_session(us, room_name, &cryptic, combination);

  return new_session;
  
}

np1secSession np1secSession::operator-(np1secSession a) {
  std::map<std::string, Participant> difference;
  std::set_difference(
    participants.begin(), participants.end(),
    a.participants.begin(), a.participants.end(),
    std::inserter(difference, difference.end()));
  np1secSession new_session(us, room_name, &cryptic, difference);

  return new_session;
}

np1secSession np1secSession::operator-(std::string leaver_nick) {
  assert(participants.find(leaver_nick) != participants.end());
  
  ParticipantMap new_session_plist = participants;
  new_session_plist.erase(leaver_nick);
  np1secSession new_session(us, room_name, &cryptic, new_session_plist, false);

  return new_session;
  
}

 /**
 * it should be invoked only once to compute the session id
 * if one need session id then they need a new session
 *
 * @return return true upon successful computation
 */
bool np1secSession::compute_session_id()
{
  assert(!session_id.get()); //if session id isn't set we have to set it
  session_id.compute(participants);
  return true;

}

/**
 *  setup session view based on session view message,
 *  note the session view is set once and for all change in 
 *  session view always need new session object.
 */
bool np1secSession::setup_session_view(np1secMessage session_view_message) {

  populate_participants_and_peers(session_view_message.session_view);
   
  compute_session_id();

  return (session_id.get() != nullptr);

}

//TODO:: add check for validity of session key
bool np1secSession::compute_session_confirmation()
{
  string to_be_hashed = Cryptic::hash_to_string_buff(session_key);
  to_be_hashed += myself.nickname;

  Cryptic::hash(to_be_hashed, session_confirmation);

  return true;
  
}

//TODO with having session confirmation it is not clear if 
//this is necessary at all but I include it for now
//as it is part of the protocol
void np1secSession::account_for_session_and_key_consistency()
{
  string to_be_hashed = Cryptic::hash_to_string_buff(session_key);
  to_be_hashed += session_id.get_as_stringbuff();

  HashBlock key_sid_hash;
  Cryptic::hash(to_be_hashed, key_sid_hash);

  last_received_message_id = 0; //key confirmation is the first message
  add_message_to_transcript(Cryptic::hash_to_string_buff(key_sid_hash),
                            last_received_message_id);

}

bool np1secSession::validate_session_confirmation(np1secMessage confirmation_message)
{
  HashBlock expected_hash;

  string to_be_hashed = Cryptic::hash_to_string_buff(session_key);
  to_be_hashed += confirmation_message.sender_nick;

  Cryptic::hash(to_be_hashed, expected_hash);

  return !(Cryptic::compare_hash(expected_hash, reinterpret_cast<const uint8_t*>(confirmation_message.session_key_confirmation.c_str())));
  
}

/**
 * compute the right secret share
 * @param side  either c_my_right = 1 or c_my_left = 1
 */
std::string np1secSession::secret_share_on(int32_t side)
{
  HashBlock hb;
  
  assert(side == c_my_left || side == c_my_right);
  uint32_t positive_side = side + ((side < 0) ? peers.size() : 0);
  unsigned int my_neighbour = (my_index + positive_side) % peers.size();

  //we can't compute the secret if we don't know the neighbour ephemeral key
  assert(participants[peers[my_neighbour]].ephemeral_key);
  if (!participants[peers[my_neighbour]].compute_p2p_private(us->long_term_key_pair.get_key_pair().first, &cryptic))
    throw np1secCryptoException();

  Cryptic::hash(Cryptic::hash_to_string_buff(participants[peers[my_neighbour]].p2p_key) + session_id.get_as_stringbuff(), hb, true);
  
  return Cryptic::hash_to_string_buff(hb);
  
}

bool np1secSession::group_enc() {
  HashBlock hbr, hbl;
  memcpy(hbr, Cryptic::strbuff_to_hash(secret_share_on(c_my_right)), sizeof(HashBlock));
  memcpy(hbl, Cryptic::strbuff_to_hash(secret_share_on(c_my_left)), sizeof(HashBlock));

  for (unsigned i=0; i < sizeof(HashBlock); i++) {
    hbr[i] ^= hbl[i];
  }

  participants[myself.nickname].set_key_share(hbr);
  return true;
  
}

bool np1secSession::group_dec() {

  std::vector<std::string> all_r(peers.size());
  HashBlock last_hbr;

  HashBlock hbr;
  memcpy(hbr, Cryptic::strbuff_to_hash(secret_share_on(c_my_right)), sizeof(HashBlock));
  size_t my_right = (my_index+c_my_right) % peers.size();
  all_r[my_index] = Cryptic::hash_to_string_buff(hbr);

  for (uint32_t counter = 0; counter < peers.size(); counter++) {
    //memcpy(all_r[my_right], last_hbr, sizeof(HashBlock));
    size_t current_peer = (my_index + counter) % peers.size();
    size_t peer_on_the_right = (current_peer + 1) % peers.size();
    all_r[current_peer] = Cryptic::hash_to_string_buff(hbr);
    for (unsigned i=0; i < sizeof(HashBlock); i++) {
        hbr[i] ^= participants[peers[peer_on_the_right]].cur_keyshare[i];
   }
  } 
  //assert(hbr[0]==reinterpret_cast<const uint8_t&>(all_r[my_index][0]));
  
  std::string to_hash;
  for (std::vector<std::string>::iterator it = all_r.begin(); it != all_r.end(); ++it) {
    to_hash += (*it).c_str();
  }

  to_hash += session_id.get_as_stringbuff();
  Cryptic::hash(to_hash.c_str(), to_hash.size(), session_key, true);
  cryptic.set_session_key(session_key);

  return true; //check if the recovered left is the same as calculated left
  
}

bool np1secSession::everybody_authenticated_and_contributed()
{
  for(ParticipantMap::iterator it = participants.begin(); it != participants.end(); it++)
    if (!it->second.authenticated || !it->second.key_share_contributed)
      return false;

  return true;
  
}

bool np1secSession::everybody_confirmed()
{
  for(vector<bool>::iterator it = confirmed_peers.begin(); it != confirmed_peers.end(); it++)
    if (!(*it))
      return false;

  return true;
  
}

/**
 *   Joiner call this after receiving the participant info to
 *    authenticate to everybody in the room
 */
bool np1secSession::joiner_send_auth_and_share() {
  assert(session_id.get()); //sanity check
  if (!group_enc()) //compute my share for group key
    return false;

  HashBlock cur_auth_token;
  std::string auth_batch;

  for(uint32_t i = 0; i < peers.size(); i++) {
    if (!participants[peers[i]].authed_to) {
      participants[peers[i]].authenticate_to(cur_auth_token, us->long_term_key_pair.get_key_pair().first, &cryptic);
      auth_batch.append(reinterpret_cast<char*>(&i), sizeof(uint32_t));
      auth_batch.append(reinterpret_cast<char*>(cur_auth_token), sizeof(HashBlock));
    }
  }

  np1secMessage outbound(&cryptic);

  outbound.create_joiner_auth_msg(session_id,
                                auth_batch,
                                string(reinterpret_cast<char*>(participants[myself.nickname].cur_keyshare), sizeof(HashBlock)));
  outbound.send(room_name, us);

  return true;
}

/**
 *  Current participant sends this to the room
 *  to re share?  (when someone leave or in
 *  session forward secrecy)
 *  TODO: when do actually we need to call this
 *  When a partcipant leave
 */
bool np1secSession::send_new_share_message() {
  assert(session_id.get());
  if (!group_enc()) //compute my share for group key
    return false;

  np1secMessage outboundmessage(&cryptic);

  outboundmessage.create_group_share_msg(session_id,
                                         string(reinterpret_cast<char*>(participants[myself.nickname].cur_keyshare), sizeof(HashBlock)));

  outboundmessage.send(room_name, us);
  return true;

}

/**
   Preparinig PARTICIPANT_INFO Message

    current user calls this to send participant info to joiner
    and others
    sid, ((U_1,y_i)...(U_{n+1},y_{i+1}), kc, z_joiner
*/
bool np1secSession::send_view_auth_and_share(string joiner_id) {
  assert(session_id.get());
  if (!group_enc()) //compute my share for group key
    return false;

  HashBlock cur_auth_token;
  if (!joiner_id.empty())
    if (!participants[joiner_id].authed_to)
      participants[joiner_id].authenticate_to(cur_auth_token, us->long_term_key_pair.get_key_pair().first, &cryptic);

  UnauthenticatedParticipantList session_view_list = session_view();
  np1secMessage outboundmessage(&cryptic);

  outboundmessage.create_participant_info_msg(session_id,
                                              session_view_list,
                                string(reinterpret_cast<char*>(cur_auth_token), sizeof(HashBlock)),
                                string(reinterpret_cast<char*>(participants[myself.nickname].cur_keyshare), sizeof(HashBlock)));

  outboundmessage.send(room_name, us);
  return true;

}

//DEPRICATED in favor of send_view_auth_share_message
/**
   Current user will use this to inform new user
   about their share and also the session plist klist

*/
// bool np1secSession::send_share_message() {
//   assert(session_id_is_set);
//   if (!group_enc()) //compute my share for group key
//     return false;
  
//   np1secMessage outboundmessage.create_participant_info(RE_SHARE,
//                                                         sid,
//                                                         //unauthenticated_participants
//                                                         //"",//auth_batch,
//                                                         session_key_share);
//   outboundmessage.send();
//   return true;

// }

/**
 * Receives the pre-processed message and based on the state
 * of the session decides what is the appropriate action
 *
 * @param receive_message pre-processed received message handed in by receive function
 *
 * @return true if state has been change 
 */
RoomAction np1secSession::state_handler(np1secMessage received_message)
{
  if (np1secFSMGraphTransitionMatrix[my_state][received_message.message_type]) //other wise just ignore
    {
      StateAndAction result  = (this->*np1secFSMGraphTransitionMatrix[my_state][received_message.message_type])(received_message);
      my_state = result.first;
      return result.second;
    }
  
  return RoomAction(RoomAction::BAD_ACTION);

}

//***** Joiner state transitors ****

/**
   For join user calls this when receivemessage has type of PARTICIPANTS_INFO
   
   sid, ((U_1,y_i)...(U_{n+1},y_{i+1}), (kc_{sender, joiner}), z_sender

   - Authenticate sender if fail halt

   for everybody including the sender

   joiner should:
   - set session view
   - compute session_id
   - add z_sender to the table of shares 
   - compute kc = kc_{joiner, everybody}
   - compute z_joiner
   - send 
   sid, ((U_1,y_i)...(U_{n+1},y_{i+1}), kc, z_joiner
*/
np1secSession::StateAndAction np1secSession::auth_and_reshare(np1secMessage received_message) {
  if (!session_id.get()) {
    if (!setup_session_view(received_message)) {
      return StateAndAction(DEAD, RoomAction());
    } else {
      if (!joiner_send_auth_and_share())
        return StateAndAction(DEAD, RoomAction());
    }
  }

  if (participants.find(received_message.sender_nick) == participants.end())
    return StateAndAction(DEAD, RoomAction());

  if (!participants[received_message.sender_nick].be_authenticated(myself.id_to_stringbuffer(), reinterpret_cast<const uint8_t*>(received_message.key_confirmation.c_str()), us->long_term_key_pair.get_key_pair().first, &cryptic)) {
    assert(0);
    return StateAndAction(DEAD, RoomAction());
  }
  
  //keep participant's z_share if they passes authentication
  participants[received_message.sender_nick].set_key_share(reinterpret_cast<const uint8_t*>(received_message.z_sender.c_str()));

  return send_session_confirmation_if_everybody_is_contributed();

  //TODO: check the ramification of lies by other participants about honest
  //participant ephemeral key. Normally nothing should happen as we recompute
  //the session id and so the session will never get messages from honest
  //participants and so will never be authed.

}

/**
   For the joiner user, calls it when receive a session confirmation
   message.
   
   sid, ((U_1,y_i)...(U_{n+1},y_{i+1}), hash(GroupKey, U_sender)
   
   of SESSION_CONFIRMATION type
   
   if it is the same sid as the session id, marks the confirmation in 
   the confirmation list for the sender. If all confirmed, change 
   state to IN_SESSION, call the call back join from ops.
   
   If the sid is different send a new join request
   
*/
np1secSession::StateAndAction np1secSession::confirm_or_resession(np1secMessage received_message) {
  //if sid is the same mark the participant as confirmed
  //receiving mismatch sid basically means rejoin
  if (Cryptic::compare_hash(received_message.session_id.get(), session_id.get())) {
    if (validate_session_confirmation(received_message))
      confirmed_peers[participants[received_message.sender_nick].index] = true;
    else {
      return StateAndAction(DEAD, RoomAction(RoomAction::NO_ACTION));
    }

    if (everybody_confirmed())
      return StateAndAction(IN_SESSION, RoomAction(RoomAction::NO_ACTION));
    
  }
  
  //Depricated: in the new model it is the np1sec room which check for
  //non matching sid and make a new session for it.
  // else {
  //   //we need to rejoin, categorically we are against chanigng session id
  //   //so we make a new session. This make us safely ignore replies to
  //   //old session id (they go to the dead session)
  //   np1secSession* new_child_session = new np1secSession(room_name); //calling join constructor;
  //   if (new_child_session->session_id_is_set) {
  //     new_child_session->my_parent = this;
  //     my_children[new_child_session->session_id] = new_child_session;
  //   }
  //   return StateAndAction(DEAD, RoomAction(RoomAction::NO_ACTION));
  // }

  return StateAndAction(my_state, RoomAction(RoomAction::NO_ACTION));
        
}

//*****JOINER state transitors END*****

//*****Current participant state transitors*****
/**
     For the current user, calls it when receive JOIN_REQUEST with
     
     (U_joiner, y_joiner)

     - start a new new participant list which does
     
     - computes session_id
     - new session does:
     - compute kc = kc_{joiner, everybody}
     - compute z_sender (self)
     - set new session status to REPLIED_TO_NEW_JOIN
     - send

     sid, ((U_1,y_i)...(U_{n+1},y_{i+1}), (kc_{sender, joiner}), z_sender
     
     of PARTICIPANT_INFO message type

     change status to REPLIED_TO_NEW_JOIN

 */
np1secSession::StateAndAction np1secSession::init_a_session_with_new_plist(np1secMessage received_message)
{

  RoomAction new_session_action;
  np1secSession* new_child_session = new np1secSession(us, room_name, &cryptic, received_message, participants);
  
  if (!new_child_session->session_id.get()) 
    delete new_child_session; //failed to establish a legit session
  else {
    new_session_action.action_type = RoomAction::NEW_SESSION;
    new_session_action.bred_session = new_child_session;

    //This broadcast not happens in session constructor because sometime we want just to make
    //a session object and not tell the whole world about it.
  }
   //Depricated: np1secRoom manages the set of sessions
   //new_child_session->my_parent = this;
   //my_children[new_child_session->session_id] = new_child_session;

   //TODO: this is incomplete, you need to report your session 
   //to the room. more logically the room just need to request the
   //creation of the room.
   // }
   // else {*
   //   //throw the session out
   //   //trigger the session (in-lmbo) about the repeated join/
   //   */
    
  //our state doesn't need to change
  return StateAndAction(my_state, new_session_action);

}

/**
 * for immature leave when we don't have leave intention 
 */
RoomAction np1secSession::shrink(std::string leaving_nick)
{

  //we are basically running the intention to leave message
  //without broadcasting it (because it is not us who intend to do so)
  //make a fake intention to leave message but don't send ack
  RoomAction new_session_action;

  np1secSession* new_child_session = new np1secSession(*this - leaving_nick);
  
  if (!new_child_session->session_id.get()) 
    delete new_child_session; //failed to establish a legit session
  else {
    new_session_action.action_type = RoomAction::NEW_SESSION;
    new_session_action.bred_session = new_child_session;
  }
    
  //we are as we have farewelled
  my_state = FAREWELLED;
  return new_session_action;

}

/**
 * Move the session from DEAD to 
 */
// RoomAction np1secSession::revive_session()
// {
//   StateAndAction state_and_action = send_session_confirmation_if_everybody_is_contributed();
//   my_state = state_and_action.first;

//   return state_and_action.second;
  
// }

/**
   For the current user, calls it when receive JOINER_AUTH
   
   sid, U_sender, y_i, _kc, z_sender, signature

   or PARTICIPANT_INFO from users in the session
   
   - Authenticate joiner halt if fails
   - Change status to AUTHED_JOINER
   - Halt all sibling sessions
   
   - add z_sender to share table
   - if all share are there compute the group key send the confirmation
   
   sid, hash(GroupKey, U_sender), signature 
   
   change status GROUP_KEY_GENERATED
   otherwise no change to the status
   
*/
np1secSession::StateAndAction np1secSession::confirm_auth_add_update_share_repo(np1secMessage received_message) {
  //If the participant isn't in the list then don't bother
  if (participants.find(received_message.sender_nick) == participants.end())
    return StateAndAction(DEAD, RoomAction());

  if (received_message.message_type == np1secMessage::JOINER_AUTH) {
    if (received_message.authentication_table.find(my_index) != received_message.authentication_table.end())
      if (!participants[received_message.sender_nick].be_authenticated(myself.id_to_stringbuffer(), Cryptic::strbuff_to_hash(received_message.authentication_table[my_index]), us->long_term_key_pair.get_key_pair().first, &cryptic))  {
      return StateAndAction(DEAD, RoomAction());
    }
  }

  //TODO:: we need to check the signature of the message here
  participants[received_message.sender_nick].set_key_share(Cryptic::strbuff_to_hash(received_message.z_sender));

  return send_session_confirmation_if_everybody_is_contributed();
  //else { //assuming the message is PARTICIPANT_INFO from other in
  //session people
    
  //}
  
}
/**
 * sends session confirmation if everybody is contributed and authenticated
 * returns DEAD state if fails to decrypt the group key.
 *         GROUP_KEY_GENERATED otherwise
 *          the current state if not everybody authenticated
 */
np1secSession::StateAndAction np1secSession::send_session_confirmation_if_everybody_is_contributed()
{
  if (everybody_authenticated_and_contributed()) {
    if (group_dec()) {
      //first compute the confirmation
      compute_session_confirmation();
      //now send the confirmation message
      np1secMessage outboundmessage(&cryptic);

      outboundmessage.create_session_confirmation_msg(session_id,
                                    Cryptic::hash_to_string_buff(session_confirmation));
      
      outboundmessage.send(room_name, us);

      return StateAndAction(GROUP_KEY_GENERATED, RoomAction());
    }

    return StateAndAction(DEAD, RoomAction());
    
  }
  //otherwise just wait for more shares
  return StateAndAction(my_state, RoomAction());

}

/**
   For the current user, calls it when receive a session confirmation
   message.
   
   sid, hash(GroupKey, U_sender), signature
   
   if it is the same sid as the session id, marks the confirmation in 
   the confirmation list for the sender. If all confirmed, change 
   state to IN_SESSION, make this session the main session of the
   room
   
   If the sid is different, something is wrong halt drop session
   
*/
np1secSession::StateAndAction np1secSession::mark_confirmed_and_may_move_session(np1secMessage received_message) {
  //TODO:realistically we don't need to check sid, if sid
  //doesn't match we shouldn't have reached this point
  if (!validate_session_confirmation(received_message))
    return StateAndAction(DEAD, c_no_room_action);
  
  confirmed_peers[participants[received_message.sender_nick].index] = true;
  
  if (everybody_confirmed()) {
    //activate(); it is matter of changing to IN_SESSION
    //we also need to initiate the transcript chain with 
    account_for_session_and_key_consistency();

    return StateAndAction(IN_SESSION, c_no_room_action);
  }

  return StateAndAction(my_state, c_no_room_action);
    
}

/**
 * This will be called when another user leaves a chatroom to update the key.
 * 
 * This should send a message the same an empty meta message for sending
 * the leaving user the status of transcript consistency
 * 
 * This also make new session which send message of Of FAREWELL type new
 * share list for the shrinked session 
 *
 * sid, z_sender, transcript_consistency_stuff
 *
 * kills all sibling sessions in making as the leaving user is no longer 
 * available to confirm any new session.
 * 
 * The status of the session is changed to farewelled. 
 * The statatus of new sid session is changed to re_shared
 */
np1secSession::StateAndAction np1secSession::send_farewell_and_reshare(np1secMessage received_message) {

  //send a farewell message
  send("", np1secMessage::JUST_ACK); //no point to send FS loads as the session is
  //ending anyway
  return init_a_session_with_new_plist(received_message);

}

/**
 * compute the id of a potential session when leaving_nick leaves the session
 */
SessionId np1secSession::shrank_session_id(std::string leaver_nick)
{
  assert(participants.find(leaver_nick) != participants.end());
  ParticipantMap temp_plist = participants;
  temp_plist.erase(leaver_nick);

  SessionId shrank_id(temp_plist);

  return shrank_id;
  
}

//Depricated: room take care of join and make a new session
// bool np1secSession::join(LongTermIDKey long_term_id_key) {
//   //don't come here
//   assert(0);
//   //We need to generate our ephemerals anyways
//   if (!cryptic.init()) {
//     return false;
//   }
//   myself.ephemeral_key = cryptic.get_ephemeral_pub_key();

//   //we add ourselves to the (authenticated) participant list
//   participants[myself.id];
//   peers[0]=myself.id;

//   // if nobody else is in the room have nothing to do more than
//   // just computing the session_id
//   if (session_view().size()== 1) {
//     assert(this->compute_session_id());
         
//   }
//   else {
    
//   }
//   us->ops->send_bare(room_name, us->user_nick(), "testing 123", NULL);
//   return true;
// }

// bool np1secSession::accept(std::string new_participant_id) {
//   UNUSED(new_participant_id);
//   return true;
// }

//TODO: this blong to message class
bool np1secSession::received_p_list(std::string participant_list) {
  //Split up participant list and load it into the map
  assert(0);
  char* tmp = strdup(participant_list.c_str());

  std::string ids_keys = strtok(tmp, c_np1sec_delim.c_str());
  std::vector<std::string> list_ids_keys;
  while (!ids_keys.empty()) {
    std::string decoded = "";
    otrl_base64_otr_decode(ids_keys.c_str(),
                           (unsigned char**)decoded.c_str(),
                           reinterpret_cast<size_t*>(ids_keys.size()));
    list_ids_keys.push_back(ids_keys);
    ids_keys = strtok(NULL, c_np1sec_delim.c_str());
  }

  for (std::vector<std::string>::iterator it = list_ids_keys.begin();
       it != list_ids_keys.end(); ++it) {
    tmp = strdup((*it).c_str());
    std::string id = strtok(tmp, c_np1sec_delim.c_str());
    std::string key = strtok(NULL, c_np1sec_delim.c_str());
    gcry_sexp_t sexp_key = Cryptic::convert_to_sexp(key); 
    Participant p();//id, crypto);
    //p.ephemeral_key = sexp_key;
    //session_view().push_back(UnauthenticatedParticipant(ParticipantId(id, p)));
  }

  return true;
}

bool np1secSession::leave() {
  //tell everybody I'm leaving and tell them about the closure
  //of my transcript consistency

  //if you are the only person in the session then
  //just leave
  //TODO:: is it good to call the ops directly?
  if (participants.size() == 1) {
    assert(my_index == 0 && peers.size() == 1);
    peers.pop_back();
    us->ops->leave(room_name, peers, us->ops->bare_sender_data);
    my_state = DEAD;
    return true;
  }

  //otherwise, inform others in the room about your leaving the room
  leave_parent = last_received_message_id;
  send("", np1secMessage::LEAVE_MESSAGE);

  farewell_deadline_timer = us->ops->set_timer(cb_leave, this, us->ops->c_inactive_ergo_non_sum_interval);
  my_state = LEAVE_REQUESTED;

  return true;

}

void np1secSession::restart_heartbeat_timer() {
  if (heartbeat_timer)
    us->ops->axe_timer(heartbeat_timer);
  
  heartbeat_timer = us->ops->set_timer(cb_send_heartbeat, this, us->ops->c_heartbeating_interval);

}

/**
 * When we receive a message we set a timer so to check that
 * everybody else has received the same message
 */
void np1secSession::start_ack_timers(np1secMessage received_message) {
  for (ParticipantMap::iterator it = participants.begin();
       it != participants.end();
       ++it) {
    //we accumulate the timers, when we receive ack, we drop what we
    //have before 
    if ((received_message.sender_nick != (*it).second.id.nickname) &&
        (received_message.sender_nick != myself.nickname)) //not for the sender and not 
      //for myself
    {
      received_transcript_chain[received_message.message_id][(*it).second.index].ack_timer_ops.session = this;
      received_transcript_chain[received_message.message_id][(*it).second.index].ack_timer_ops.participant = &(it->second);
      received_transcript_chain[received_message.message_id][(*it).second.index].ack_timer_ops.message_id = received_message.message_id;
      received_transcript_chain[received_message.message_id][(*it).second.index].consistency_timer = us->ops->set_timer(cb_ack_not_received, &(received_transcript_chain[received_message.message_id][(*it).second.index].ack_timer_ops), us->ops->c_consistency_failure_interval);
      
    }
  }
}

/**
 * When we send a message we start a timer to make sure that we'll
 * receive the message from the server
 */
void np1secSession::start_receive_ack_timer() {
  //if there is already an ack timer 
  //then that will take care of acking 
  //for us as well
  if (!send_ack_timer) {
    send_ack_timer = us->ops->set_timer(cb_send_ack, this, us->ops->c_ack_interval);
  }
  
}

void np1secSession::stop_timer_send() {
  for (std::map<std::string, Participant>::iterator
       it = participants.begin();
       it != participants.end();
       ++it) {
    if ((*it).second.send_ack_timer) {
      us->ops->axe_timer((*it).second.send_ack_timer);
      send_ack_timer = nullptr;
    }
  }
}

void np1secSession::stop_timer_receive(std::string acknowledger_id, MessageId message_id) {

  for(MessageId i = participants[acknowledger_id].last_acked_message_id + 1; i <= message_id; i++) {
      us->ops->axe_timer(received_transcript_chain[message_id][participants[acknowledger_id].index].consistency_timer);
      received_transcript_chain[message_id][participants[acknowledger_id].index].consistency_timer = nullptr;
  }

  participants[acknowledger_id].last_acked_message_id = message_id;

}

/**
 * Inserts a block in the send transcript chain and start a 
 * timer to receive the ack for it
 */
void np1secSession::update_send_transcript_chain(MessageId own_message_id,
                                  std::string message) {
  HashBlock hb;
  Cryptic::hash(message, hb, true);
  sent_transcript_chain[own_message_id].transcript_hash = Cryptic::hash_to_string_buff(hb);
  sent_transcript_chain[own_message_id].ack_timer_ops = AckTimerOps(this, nullptr, own_message_id);
 
  sent_transcript_chain[own_message_id].consistency_timer = us->ops->set_timer(cb_ack_not_sent, &(sent_transcript_chain[own_message_id].ack_timer_ops), us->ops->c_send_receive_interval);

}

/**
 * - kills the send ack timer for the message in case we are the sender
 * - Fill our own transcript chain for the message
 * - start all ack timer for others for this message
 * - Perform parent consistency check
 */
void np1secSession::perform_received_consisteny_tasks(np1secMessage received_message)
{
  //defuse the "I didn't get my own message timer 
  if (received_message.sender_nick == myself.nickname) {
    assert(sent_transcript_chain.find(received_message.sender_message_id) != sent_transcript_chain.end()); //if the signature isn't failed and we don't have record of sending this then something is terribly wrong;
    us->ops->axe_timer(sent_transcript_chain[received_message.sender_message_id].consistency_timer);
    sent_transcript_chain[received_message.sender_message_id].consistency_timer = nullptr;}

  add_message_to_transcript(received_message.final_whole_message, received_message.message_id);

}

/**
 * - check the consistency of the parent message with our own.
 * - kill all ack receive timers of the sender for the parent backward
 */
void np1secSession::check_parent_message_consistency(np1secMessage received_message)
{
  received_transcript_chain[received_message.parent_id][participants[received_message.sender_nick].index].transcript_hash = received_message.transcript_chain_hash;

  if (received_transcript_chain[received_message.parent_id][my_index].transcript_hash != received_transcript_chain[received_message.parent_id][participants[received_message.sender_nick].index].transcript_hash)
    {
      std::string consistancy_failure_message = received_message.sender_nick  + " transcript doesn't match ours as of " + to_string(received_message.parent_id);
      us->ops->display_message(room_name, "np1sec directive", consistancy_failure_message, us);
    }

  stop_timer_receive(received_message.sender_nick, received_message.message_id);

}

/**
 * - check the consistency of all participants for the parent leave message
 */
bool np1secSession::check_leave_transcript_consistency()
{
  uint32_t no_of_peers_farewelled = 0;
  if (received_transcript_chain.find(leave_parent) != received_transcript_chain.end()) {
    for(uint32_t i = 0; i < peers.size(); i++) {
     
      //we need to check if we have already got the farewell from this peer
      if (!received_transcript_chain[leave_parent][i].transcript_hash.empty()) {
        no_of_peers_farewelled++;
        if (received_transcript_chain[leave_parent][i].transcript_hash != received_transcript_chain[leave_parent][my_index].transcript_hash) {
          std::string consistency_failure_message = peers[i]  + " transcript doesn't match ours";
          us->ops->display_message(room_name, "np1sec directive", consistency_failure_message, us);
        } //not equal
      } //not empty
    } //for
  } //we got it already
  
  return (no_of_peers_farewelled == peers.size());
  
}

void np1secSession::add_message_to_transcript(std::string message,
                                        MessageId message_id) {
  HashBlock hb;
  std::stringstream ss;
  std::string pointlessconversion;

  if (received_transcript_chain.size() > 0) {
    ss << received_transcript_chain.rbegin()->second[my_index].transcript_hash;
    ss >> pointlessconversion;
    pointlessconversion += c_np1sec_delim + message;

  } else {
    pointlessconversion = message;

  }

  Cryptic::hash(pointlessconversion, hb);

  if (received_transcript_chain.find(message_id) == received_transcript_chain.end()) {
    ConsistencyBlockVector chain_block(participants.size());
    received_transcript_chain.insert(pair<MessageId, ConsistencyBlockVector>(message_id, chain_block));
  }
  
  (received_transcript_chain[message_id])[my_index].transcript_hash = Cryptic::hash_to_string_buff(hb);
  received_transcript_chain[message_id][my_index].consistency_timer = nullptr;

}

bool np1secSession::send(std::string message, np1secMessage::np1secMessageSubType message_type) {
  own_message_counter++;
  // TODO(bill)
  // Add code to check message type and get
  // meta load if needed
  np1secMessage outbound(&cryptic);

  outbound.create_in_session_msg(session_id, 
                                 my_index,
                                 own_message_counter,
                                 last_received_message_id,
                                 received_transcript_chain.rbegin()->second[my_index].transcript_hash,
                                 message_type,
                                 message
                                 //no in session forward secrecy for now
                           );

  update_send_transcript_chain(own_message_counter, outbound.compute_hash());
  // As we're sending a new message we are no longer required to ack
  // any received messages
  stop_timer_send();

  if (message_type == np1secMessage::USER_MESSAGE)  {
    // We create a set of times for all other peers for acks we expect for
    // our sent message
    start_receive_ack_timer(); //If you are overwritng
    //timers then you need plan of recourse.
  }

  // us->ops->send_bare(room_name, outbound);
  outbound.send(room_name, us);
  return true;
  
}

np1secSession::StateAndAction np1secSession::receive(np1secMessage encrypted_message) {

  //we need to receive it again, as now we have the encryption key 
  np1secMessage received_message(encrypted_message.final_whole_message, &cryptic, participants.size());

  //check signature if not valid, just ignore the message
  //first we need to get the correct ephemeral key
  if (received_message.sender_index < peers.size()) {
    if (received_message.verify_message(participants[peers[received_message.sender_index]].ephemeral_key)) {
      //only messages with valid signature are concidered received
      //for any matters including consistency chcek
      last_received_message_id++;
      received_message.sender_nick = peers[received_message.sender_index]; //just to keep the message structure consistent, and for the use in new session (like session resulted from leave) otherwise in the session we should just use the index
      perform_received_consisteny_tasks(received_message);
      if (my_state == LEAVE_REQUESTED) {
        if (check_leave_transcript_consistency()) {//we are done we can leave
          //stop the farewell deadline timer
          if (farewell_deadline_timer) {
            us->ops->axe_timer(farewell_deadline_timer);
            farewell_deadline_timer = nullptr;
          }

          peers.pop_back();
          us->ops->leave(room_name, peers, us->ops->bare_sender_data);
          StateAndAction(DEAD, c_no_room_action);
          
        }
      }
      //if it is user message, display content
      else if ((received_message.message_sub_type == np1secMessage::USER_MESSAGE)) {
        us->ops->display_message(room_name, participants[peers[received_message.sender_index]].id.nickname, received_message.user_message, us->ops->bare_sender_data);
      }
      else if ((received_message.message_sub_type == np1secMessage::LEAVE_MESSAGE) && (received_message.sender_nick != myself.nickname))  {
        return send_farewell_and_reshare(received_message);
      }
    
    } else 
      received_message.message_type = np1secMessage::INADMISSIBLE;
  } else
    received_message.message_type = np1secMessage::INADMISSIBLE;



  // if (*transcript_chain_hash == received_message.transcript_chain_hash) {
  //   add_message_to_transcript(received_message.user_message,
  //                       received_message.message_id);
  //   // Stop awaiting ack timer for the sender
  //   stop_timer_receive(received_message.sender_nick, received_message.message_id);

  //   // Start an ack timer for us so we remember to say thank you
  //   // for the message
  //   start_receive_ack_timer(received_message.sender_nick);

  // } else {
  //   // The hash is a lie!
  // }

  // if (received_message.message_type == np1secMessage::SESSION_P_LIST) {
  //   //TODO
  //   // function to separate peers
  //   // add peers to map
  //   // convert load to sexp
  // }
  return StateAndAction(IN_SESSION, c_no_room_action);

}

/**
 * Decides what load to include in the current message
 */
np1secMessage::np1secMessageSubType np1secSession::forward_secrecy_load_type()
{
  return np1secMessage::JUST_ACK;
  //throw np1secNotImplementedException();
}

np1secSession::~np1secSession() {
  //delete session_id;
  //return;
}

