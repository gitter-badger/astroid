# include <iostream>
# include <string>

# include <notmuch.h>
# include <gmime/gmime.h>
# include <boost/filesystem.hpp>

# include "astroid.hh"
# include "db.hh"
# include "log.hh"
# include "message_thread.hh"
# include "chunk.hh"
# include "utils/utils.hh"
# include "utils/date_utils.hh"
# include "utils/address.hh"
# include "utils/ustring_utils.hh"

using namespace std;
using namespace boost::filesystem;

namespace Astroid {
  /* --------
   * Message
   * --------
   */
  Message::Message () {
    in_notmuch = false;
    has_file   = false;
  }

  Message::Message (ustring _fname) : fname (_fname) {

    log << info << "msg: loading message from file: " << fname << endl;
    in_notmuch = false;
    has_file   = true;
    load_message_from_file (fname);
  }

  Message::Message (ustring _mid, ustring _fname) {
    mid = _mid;
    fname = _fname;
    log << info << "msg: loading message from file (mid supplied): " << fname << endl;
    in_notmuch = false;
    has_file   = true;
    load_message_from_file (fname);
  }

  Message::Message (notmuch_message_t *message, int _level) {
    /* The caller must make sure the message pointer
     * is valid and not destroyed while initializing */

    mid = notmuch_message_get_message_id (message);
    in_notmuch = true;
    has_file   = true;
    level      = _level;

    log << info << "msg: loading mid: " << mid << endl;

    fname = notmuch_message_get_filename (message);
    log << info << "msg: filename: " << fname << endl;

    load_message_from_file (fname);
    load_tags (message);
  }

  Message::Message (GMimeMessage * _msg) {
    log << info << "msg: loading message from GMimeMessage." << endl;
    in_notmuch = false;
    has_file   = false;

    load_message (_msg);
  }

  Message::~Message () {
    //g_object_unref (message);
  }

  void Message::load_tags (Db * db) {
    if (!in_notmuch) {
      log << error << "mt: error: message not in database." << endl;
      throw invalid_argument ("mt: load_tags on message not in database.");
    }

    if (mid == "") {
      log << error << "mt: error: mid not defined, no tags" << endl;
      throw invalid_argument ("mt: load_tags on message without message id.");
    } else {
      /* get tags from nm db */

      lock_guard<Db> grd (*db);
      db->on_message (mid, [&](notmuch_message_t * msg)
        {
          load_tags (msg);
        });
    }
  }

  void Message::load_tags (notmuch_message_t * msg) {

    tags.clear ();

    notmuch_tags_t * ntags;
    for (ntags = notmuch_message_get_tags (msg);
         notmuch_tags_valid (ntags);
         notmuch_tags_move_to_next (ntags)) {

      ustring t = ustring(notmuch_tags_get (ntags));
      tags.push_back (t);
    }

  }

  void Message::load_message_from_file (ustring fname) {
    GMimeStream   * stream  = g_mime_stream_file_new_for_path (fname.c_str(), "r");
    if (stream == NULL) {
      if (!exists (fname.c_str())) {
        log << error << "failed to open file: " << fname << ", it does not exist!" << endl;
      } else {
        log << error << "failed to open file: " << fname << " (unspecified error)" << endl;
      }

      string error_s = "failed to open file: " + fname;
      throw message_error (error_s.c_str());

    }

    GMimeParser   * parser  = g_mime_parser_new_with_stream (stream);

    GMimeMessage * _message = g_mime_parser_construct_message (parser);

    load_message (_message);

    g_object_unref (stream); // reffed from parser
    g_object_unref (parser); // reffed from message
  }

  void Message::load_message (GMimeMessage * _msg) {

    /* Load message with parts.
     *
     * example:
     * https://git.gnome.org/browse/gmime/tree/examples/basic-example.c
     *
     *
     * Do this a bit like sup:
     * - build up a tree/list of chunks that are viewable, except siblings.
     * - show text and html parts
     * - show a gallery of attachments at the bottom
     *
     */

    message = _msg;

    if (mid == "") {
      mid = g_mime_message_get_message_id (message);
    }

    /* read header fields */
    const char *c;
    c  = g_mime_message_get_sender (message);
    if (c != NULL) sender = ustring (c);
    else sender = "";

    c = g_mime_message_get_subject (message);
    if (c != NULL) subject = ustring (c);
    else subject = "";

    c = g_mime_object_get_header (GMIME_OBJECT(message), "In-Reply-To");
    if (c != NULL) inreplyto = ustring (c);
    else inreplyto = "";

    c = g_mime_object_get_header (GMIME_OBJECT(message), "References");
    if (c != NULL) references = ustring (c);
    else references = "";

    c = g_mime_object_get_header (GMIME_OBJECT(message), "Reply-To");
    if (c != NULL) reply_to = ustring (c);
    else reply_to = "";

    g_mime_message_get_date (message, &received_time, NULL);

    root = refptr<Chunk>(new Chunk (g_mime_message_get_mime_part (message)));

    g_object_ref (message);  // TODO: a little bit at loss here -> change to
                             //       std::something.
  }

  ustring Message::viewable_text (bool html, bool fallback_html) {
    /* build message body:
     * html:      output html (using gmimes html filter)
     *
     */

    if (html && fallback_html) {
      throw logic_error ("message: html implies fallback_html");
    }

    ustring body;

    function< void (refptr<Chunk>) > app_body =
      [&] (refptr<Chunk> c)
    {
      /* check if we're the preferred sibling */
      bool use = false;

      if (c->siblings.size() >= 1) {
        if (c->preferred) {
          use = true;
        } else {
          /* check if there are any other preferred */
          if (all_of (c->siblings.begin (),
                      c->siblings.end (),
                      [](refptr<Chunk> c) { return (!c->preferred); })) {
            use = true;
          } else {
            use = false;
          }
        }
      } else {
        use = true;
      }

      if (use) {
        if (c->viewable && (c->preferred || html || fallback_html)) {
          body += c->viewable_text (html);
        }

        for_each (c->kids.begin(),
                  c->kids.end (),
                  app_body);
      }
    };

    app_body (root);

    return body;
  }

  vector<refptr<Chunk>> Message::attachments () {
    /* return a flat vector of attachments */

    vector<refptr<Chunk>> attachments;

    function< void (refptr<Chunk>) > app_attachment =
      [&] (refptr<Chunk> c)
    {
      if (c->attachment)
        attachments.push_back (c);

      for_each (c->kids.begin(),
                c->kids.end (),
                app_attachment);
    };

    app_attachment (root);

    return attachments;
  }

  refptr<Chunk> Message::get_chunk_by_id (int id) {
    if (root->id == id) {
      return root;
    } else {
      return root->get_by_id (id);
    }
  }

  vector<refptr<Chunk>> Message::mime_messages () {
    /* return a flat vector of mime messages */

    vector<refptr<Chunk>> mime_messages;

    function< void (refptr<Chunk>) > app_mm =
      [&] (refptr<Chunk> c)
    {
      if (c->mime_message)
        mime_messages.push_back (c);

      for_each (c->kids.begin(),
                c->kids.end (),
                app_mm);
    };

    app_mm (root);

    return mime_messages;
  }

  ustring Message::date () {
    return ustring (g_mime_message_get_date_as_string (message));
  }

  ustring Message::pretty_date () {
    return Date::pretty_print (received_time);
  }

  ustring Message::pretty_verbose_date () {
    return Date::pretty_print_verbose (received_time);
  }

  InternetAddressList * Message::to () {
    return g_mime_message_get_recipients (message, GMIME_RECIPIENT_TYPE_TO);
  }

  InternetAddressList * Message::cc () {
    return g_mime_message_get_recipients (message, GMIME_RECIPIENT_TYPE_CC);
  }

  InternetAddressList * Message::bcc () {
    return g_mime_message_get_recipients (message, GMIME_RECIPIENT_TYPE_BCC);
  }

  AddressList Message::all_to_from () {
    return ( AddressList(to()) + AddressList(cc()) + AddressList(bcc()) + Address(sender) );
  }

  ustring Message::get_filename (ustring appendix) {
    ustring _f = root->get_filename ();

    if (_f.size () == 0) {
      _f = Utils::safe_fname (subject);

      if (is_patch ()) {

        if (_f.substr(0,5).uppercase() == "PATCH") _f.erase (0, 5);

        // safe_fname will catch any double _

        if (appendix.size ()) {
          _f += "-" + appendix;
        }

        _f += ".patch";

      } else {

        if (appendix.size ()) {
          _f += "-" + appendix;
        }

        _f += ".eml";
      }
    }

    _f = Utils::safe_fname (_f);

    return _f;
  }

  void Message::save () {
    Gtk::FileChooserDialog dialog ("Save message..",
        Gtk::FILE_CHOOSER_ACTION_SAVE);

    dialog.add_button ("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button ("_Select", Gtk::RESPONSE_OK);
    dialog.set_do_overwrite_confirmation (true);

    ustring _f = get_filename ();

    dialog.set_current_name (_f);

    int result = dialog.run ();

    switch (result) {
      case (Gtk::RESPONSE_OK):
        {
          string fname = dialog.get_filename ();

          save_to (fname);

          break;
        }

      default:
        {
          log << debug << "msg: save: cancelled." << endl;
        }
    }
  }

  void Message::save_to (ustring tofname) {
    // apparently boost needs to be compiled with -std=c++0x
    // https://svn.boost.org/trac/boost/ticket/6124
    // copy_file ( path (fname), path (tofname) );

    path to (tofname.c_str());
    if (is_directory (to)) {
      ustring nfname = get_filename ();
      path newto = to / path (nfname.c_str());

      while (exists (newto)) {
        nfname = get_filename (UstringUtils::random_alphanumeric (5));

        newto = to / path(nfname.c_str ());
      }

      to = newto;
    }

    tofname = ustring (to.c_str());
    log << info << "msg: saving to: " << tofname << endl;

    if (has_file)
    {
      ifstream src (fname, ios::binary);
      ofstream dst (tofname, ios::binary);

      if (!src.good () || !dst.good ()) {
        log << error << "msg: failed writing to: " << tofname << endl;
        return;
      }

      dst << src.rdbuf ();
    } else {
      /* write GMimeMessage */

      FILE * MessageFile = fopen(tofname.c_str(), "w");
      GMimeStream * stream = g_mime_stream_file_new(MessageFile);
      g_mime_object_write_to_stream(GMIME_OBJECT(message), stream);
      g_object_unref(stream);

    }
  }

  refptr<Glib::ByteArray> Message::contents () {
    return root->contents ();
  }

  bool Message::is_patch () {
    return (
        (subject.substr(0,3).uppercase() != "RE:") &&
         Glib::Regex::match_simple ("\\[PATCH.*\\]", subject));
  }

  /************
   * exceptions
   * **********
   */

  message_error::message_error (const char * w) : runtime_error (w)
  {
  }

  /* --------
   * MessageThread
   * --------
   */

  MessageThread::MessageThread () {
    in_notmuch = false;
  }

  MessageThread::MessageThread (refptr<NotmuchThread> _nmt) : thread (_nmt) {
    in_notmuch = true;

  }

  void MessageThread::load_messages (Db * db) {
    /* update values */
    subject     = thread->subject;
    /*
    newest_date = notmuch_thread_get_newest_date (nm_thread);
    unread      = check_unread (nm_thread);
    attachment  = check_attachment (nm_thread);
    */


    /* get messages from thread */
    db->on_thread (thread->thread_id, [&](notmuch_thread_t * nm_thread)
      {

        notmuch_messages_t * qmessages;
        notmuch_message_t  * message;

        int level = 0;

        function<void(notmuch_message_t *, int)> add_replies =
          [&] (notmuch_message_t * root, int lvl) {

          notmuch_messages_t * replies;
          notmuch_message_t  * reply;

          for (replies = notmuch_message_get_replies (root);
               notmuch_messages_valid (replies);
               notmuch_messages_move_to_next (replies)) {


              reply = notmuch_messages_get (replies);
              messages.push_back (refptr<Message> (new Message (reply, lvl)));

              add_replies (reply, lvl + 1);

            }

          };

        for (qmessages = notmuch_thread_get_toplevel_messages (nm_thread);
             notmuch_messages_valid (qmessages);
             notmuch_messages_move_to_next (qmessages)) {

          message = notmuch_messages_get (qmessages);

          messages.push_back (refptr<Message>(new Message (message, level)));

          add_replies (message, level + 1);

        }
      });
  }

  void MessageThread::add_message (ustring fname) {
    messages.push_back (refptr<Message>(new Message (fname)));
  }

  void MessageThread::add_message (refptr<Chunk> c) {
    if (!c->mime_message) {
      log << error << "mt: can only add message chunks that are GMimeMessages." << endl;
      throw runtime_error ("mt: can only add message chunks that are GMimeMessages");
    }

    messages.push_back (c->get_mime_message ());

    if (subject == "") {
      subject = (*(--messages.end()))->subject;
    }
  }


  void MessageThread::reload_messages () {
  }

}

