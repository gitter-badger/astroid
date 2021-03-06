# define BOOST_TEST_DYN_LINK
# define BOOST_TEST_MODULE TestOpenDb
# include <boost/test/unit_test.hpp>
# include <boost/filesystem.hpp>

# include "test_common.hh"
# include "db.hh"
# include "message_thread.hh"
# include "glibmm.h"
using namespace std;

using namespace boost::filesystem;

BOOST_AUTO_TEST_SUITE(DbTest)

  BOOST_AUTO_TEST_CASE(open_confirm)
  {
    setup ();
    astroid->config->config.put ("astroid.notmuch.db", "test/mail/test_mail");

    Db * db;

    BOOST_CHECK_NO_THROW ( db = new Db (Db::DbMode::DATABASE_READ_ONLY) );

    delete db;

    teardown ();
  }

  //BOOST_AUTO_TEST_CASE_EXPECTED_FAILURES( open_rw, 1 )
  BOOST_AUTO_TEST_CASE(open_rw)
  {
    setup ();
    astroid->config->config.put ("astroid.notmuch.db", "test/mail/test_mail");

    Db * db;

    BOOST_CHECK_NO_THROW ( db = new Db (Db::DbMode::DATABASE_READ_WRITE) );

    //this_thread::sleep_for (chrono::milliseconds(5000));

    // it seems like a child process is killed here, is it xapian?
    //delete db;

    teardown ();
  }

  BOOST_AUTO_TEST_CASE(open_error)
  {
    setup ();
    astroid->config->config.put ("astroid.notmuch.db", "test/mail/test_mail/non_existant");

    Db * db;

    BOOST_CHECK_THROW ( db = new Db (Db::DbMode::DATABASE_READ_ONLY), database_error );

    //delete db;

    teardown ();
  }

BOOST_AUTO_TEST_SUITE_END()

