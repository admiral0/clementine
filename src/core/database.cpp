/* This file is part of Clementine.
   Copyright 2010, David Sansome <me@davidsansome.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"
#include "database.h"
#include "scopedtransaction.h"
#include "utilities.h"
#include "core/application.h"
#include "core/logging.h"
#include "core/taskmanager.h"

#include <boost/scope_exit.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QLibrary>
#include <QLibraryInfo>
#include <QSqlDriver>
#include <QSqlQuery>
#include <QtDebug>
#include <QThread>
#include <QUrl>
#include <QVariant>

const char* Database::kDatabaseFilename = "clementine.db";
const int Database::kSchemaVersion = 43;
const char* Database::kMagicAllSongsTables = "%allsongstables";

int Database::sNextConnectionId = 1;
QMutex Database::sNextConnectionIdMutex;

Database::Token::Token(const QString& token, int start, int end)
    : token(token),
      start_offset(start),
      end_offset(end) {
}

struct sqlite3_tokenizer_module {
  int iVersion;
  int (*xCreate)(
    int argc,                           /* Size of argv array */
    const char *const*argv,             /* Tokenizer argument strings */
    sqlite3_tokenizer **ppTokenizer     /* OUT: Created tokenizer */
  );

  int (*xDestroy)(sqlite3_tokenizer *pTokenizer);

  int (*xOpen)(
    sqlite3_tokenizer *pTokenizer,       /* Tokenizer object */
    const char *pInput, int nBytes,      /* Input buffer */
    sqlite3_tokenizer_cursor **ppCursor  /* OUT: Created tokenizer cursor */
  );

  int (*xClose)(sqlite3_tokenizer_cursor *pCursor);

  int (*xNext)(
    sqlite3_tokenizer_cursor *pCursor,   /* Tokenizer cursor */
    const char **ppToken, int *pnBytes,  /* OUT: Normalized text for token */
    int *piStartOffset,  /* OUT: Byte offset of token in input buffer */
    int *piEndOffset,    /* OUT: Byte offset of end of token in input buffer */
    int *piPosition      /* OUT: Number of tokens returned before this one */
  );
};

struct sqlite3_tokenizer {
  const sqlite3_tokenizer_module *pModule;  /* The module for this tokenizer */
  /* Tokenizer implementations will typically add additional fields */
};

struct sqlite3_tokenizer_cursor {
  sqlite3_tokenizer *pTokenizer;       /* Tokenizer for this cursor. */
  /* Tokenizer implementations will typically add additional fields */
};

int (*Database::_sqlite3_value_type) (sqlite3_value*) = NULL;
sqlite_int64 (*Database::_sqlite3_value_int64) (sqlite3_value*) = NULL;
const uchar* (*Database::_sqlite3_value_text) (sqlite3_value*) = NULL;
void (*Database::_sqlite3_result_int64) (sqlite3_context*, sqlite_int64) = NULL;
void* (*Database::_sqlite3_user_data) (sqlite3_context*) = NULL;

int (*Database::_sqlite3_open) (const char*, sqlite3**) = NULL;
const char* (*Database::_sqlite3_errmsg) (sqlite3*) = NULL;
int (*Database::_sqlite3_close) (sqlite3*) = NULL;
sqlite3_backup* (*Database::_sqlite3_backup_init) (
    sqlite3*, const char*, sqlite3*, const char*) = NULL;
int (*Database::_sqlite3_backup_step) (sqlite3_backup*, int) = NULL;
int (*Database::_sqlite3_backup_finish) (sqlite3_backup*) = NULL;
int (*Database::_sqlite3_backup_pagecount) (sqlite3_backup*) = NULL;
int (*Database::_sqlite3_backup_remaining) (sqlite3_backup*) = NULL;

bool Database::sStaticInitDone = false;
bool Database::sLoadedSqliteSymbols = false;

sqlite3_tokenizer_module* Database::sFTSTokenizer = NULL;


int Database::FTSCreate(int argc, const char* const* argv, sqlite3_tokenizer** tokenizer) {
  *tokenizer = reinterpret_cast<sqlite3_tokenizer*>(new UnicodeTokenizer);

  return SQLITE_OK;
}

int Database::FTSDestroy(sqlite3_tokenizer* tokenizer) {
  UnicodeTokenizer* real_tokenizer = reinterpret_cast<UnicodeTokenizer*>(tokenizer);
  delete real_tokenizer;
  return SQLITE_OK;
}

int Database::FTSOpen(
    sqlite3_tokenizer* pTokenizer,
    const char* input,
    int bytes,
    sqlite3_tokenizer_cursor** cursor) {
  UnicodeTokenizerCursor* new_cursor = new UnicodeTokenizerCursor;
  new_cursor->pTokenizer = pTokenizer;
  new_cursor->position = 0;

  QString str = QString::fromUtf8(input, bytes).toLower();
  QChar* data = str.data();
  // Decompose and strip punctuation.
  QList<Token> tokens;
  QString token;
  int start_offset = 0;
  int offset = 0;
  for (int i = 0; i < str.length(); ++i) {
    QChar c = data[i];
    ushort unicode = c.unicode();
    if (unicode <= 0x007f) {
      offset += 1;
    } else if (unicode >= 0x0080 && unicode <= 0x07ff) {
      offset += 2;
    } else if (unicode >= 0x0800) {
      offset += 3;
    }
    // Unicode astral planes unsupported in Qt?
    /*else if (unicode >= 0x010000 && unicode <= 0x10ffff) {
      offset += 4;
    }*/

    if (!data[i].isLetterOrNumber()) {
      // Token finished.
      if (token.length() != 0) {
        tokens << Token(token, start_offset, offset - 1);
        start_offset = offset;
        token.clear();
      } else {
        ++start_offset;
      }
    } else {
      if (data[i].decompositionTag() != QChar::NoDecomposition) {
        token.push_back(data[i].decomposition()[0]);
      } else {
        token.push_back(data[i]);
      }
    }

    if (i == str.length() - 1) {
      if (token.length() != 0) {
        tokens << Token(token, start_offset, offset);
        token.clear();
      }
    }
  }

  new_cursor->tokens = tokens;
  *cursor = reinterpret_cast<sqlite3_tokenizer_cursor*>(new_cursor);

  return SQLITE_OK;
}

int Database::FTSClose(sqlite3_tokenizer_cursor* cursor) {
  UnicodeTokenizerCursor* real_cursor = reinterpret_cast<UnicodeTokenizerCursor*>(cursor);
  delete real_cursor;

  return SQLITE_OK;
}

int Database::FTSNext(
    sqlite3_tokenizer_cursor* cursor,
    const char** token,
    int* bytes,
    int* start_offset,
    int* end_offset,
    int* position) {
  UnicodeTokenizerCursor* real_cursor = reinterpret_cast<UnicodeTokenizerCursor*>(cursor);

  QList<Token> tokens = real_cursor->tokens;
  if (real_cursor->position >= tokens.size()) {
    return SQLITE_DONE;
  }

  Token t = tokens[real_cursor->position];
  QByteArray utf8 = t.token.toUtf8();
  *token = utf8.constData();
  *bytes = utf8.size();
  *start_offset = t.start_offset;
  *end_offset = t.end_offset;
  *position = real_cursor->position++;

  real_cursor->current_utf8 = utf8;

  return SQLITE_OK;
}


void Database::StaticInit() {
  if (sStaticInitDone) {
    return;
  }
  sStaticInitDone = true;

  sFTSTokenizer = new sqlite3_tokenizer_module;
  sFTSTokenizer->iVersion = 0;
  sFTSTokenizer->xCreate = &Database::FTSCreate;
  sFTSTokenizer->xDestroy = &Database::FTSDestroy;
  sFTSTokenizer->xOpen = &Database::FTSOpen;
  sFTSTokenizer->xNext = &Database::FTSNext;
  sFTSTokenizer->xClose = &Database::FTSClose;

#ifdef HAVE_STATIC_SQLITE
  // We statically link libqsqlite.dll on windows and mac so these symbols are already
  // available
  _sqlite3_value_type = sqlite3_value_type;
  _sqlite3_value_int64 = sqlite3_value_int64;
  _sqlite3_value_text = sqlite3_value_text;
  _sqlite3_result_int64 = sqlite3_result_int64;
  _sqlite3_user_data = sqlite3_user_data;

  _sqlite3_open = sqlite3_open;
  _sqlite3_errmsg = sqlite3_errmsg;
  _sqlite3_close = sqlite3_close;
  _sqlite3_backup_init = sqlite3_backup_init;
  _sqlite3_backup_step = sqlite3_backup_step;
  _sqlite3_backup_finish = sqlite3_backup_finish;
  _sqlite3_backup_pagecount = sqlite3_backup_pagecount;
  _sqlite3_backup_remaining = sqlite3_backup_remaining;

  sLoadedSqliteSymbols = true;
  return;
#else // HAVE_STATIC_SQLITE
  QString plugin_path = QLibraryInfo::location(QLibraryInfo::PluginsPath) +
                        "/sqldrivers/libqsqlite";

  QLibrary library(plugin_path);
  if (!library.load()) {
    qLog(Error) << "QLibrary::load() failed for " << plugin_path;
    return;
  }

  _sqlite3_value_type = reinterpret_cast<int (*) (sqlite3_value*)>(
      library.resolve("sqlite3_value_type"));
  _sqlite3_value_int64 = reinterpret_cast<sqlite_int64 (*) (sqlite3_value*)>(
      library.resolve("sqlite3_value_int64"));
  _sqlite3_value_text = reinterpret_cast<const uchar* (*) (sqlite3_value*)>(
      library.resolve("sqlite3_value_text"));
  _sqlite3_result_int64 = reinterpret_cast<void (*) (sqlite3_context*, sqlite_int64)>(
      library.resolve("sqlite3_result_int64"));
  _sqlite3_user_data = reinterpret_cast<void* (*) (sqlite3_context*)>(
      library.resolve("sqlite3_user_data"));

  _sqlite3_open = reinterpret_cast<int (*) (const char*, sqlite3**)>(
      library.resolve("sqlite3_open"));
  _sqlite3_errmsg = reinterpret_cast<const char* (*) (sqlite3*)>(
      library.resolve("sqlite3_errmsg"));
  _sqlite3_close = reinterpret_cast<int (*) (sqlite3*)>(
      library.resolve("sqlite3_close"));
  _sqlite3_backup_init = reinterpret_cast<
      sqlite3_backup* (*) (sqlite3*, const char*, sqlite3*, const char*)>(
          library.resolve("sqlite3_backup_init"));
  _sqlite3_backup_step = reinterpret_cast<int (*) (sqlite3_backup*, int)>(
      library.resolve("sqlite3_backup_step"));
  _sqlite3_backup_finish = reinterpret_cast<int (*) (sqlite3_backup*)>(
      library.resolve("sqlite3_backup_finish"));
  _sqlite3_backup_pagecount = reinterpret_cast<int (*) (sqlite3_backup*)>(
      library.resolve("sqlite3_backup_pagecount"));
  _sqlite3_backup_remaining = reinterpret_cast<int (*) (sqlite3_backup*)>(
      library.resolve("sqlite3_backup_remaining"));

  if (!_sqlite3_value_type ||
      !_sqlite3_value_int64 ||
      !_sqlite3_value_text ||
      !_sqlite3_result_int64 ||
      !_sqlite3_user_data ||
      !_sqlite3_open ||
      !_sqlite3_errmsg ||
      !_sqlite3_close ||
      !_sqlite3_backup_init ||
      !_sqlite3_backup_step ||
      !_sqlite3_backup_finish ||
      !_sqlite3_backup_pagecount ||
      !_sqlite3_backup_remaining) {
    qLog(Error) << "Couldn't resolve sqlite symbols";
    sLoadedSqliteSymbols = false;
  } else {
    sLoadedSqliteSymbols = true;
  }
#endif
}

Database::Database(Application* app, QObject* parent, const QString& database_name)
  : QObject(parent),
    app_(app),
    mutex_(QMutex::Recursive),
    injected_database_name_(database_name),
    query_hash_(0),
    startup_schema_version_(-1)
{
  {
    QMutexLocker l(&sNextConnectionIdMutex);
    connection_id_ = sNextConnectionId ++;
  }

  directory_ = QDir::toNativeSeparators(
      Utilities::GetConfigPath(Utilities::Path_Root));

  attached_databases_["jamendo"] = AttachedDatabase(
        directory_ + "/jamendo.db", ":/schema/jamendo.sql");

  QMutexLocker l(&mutex_);
  Connect();
}

QSqlDatabase Database::Connect() {
  QMutexLocker l(&connect_mutex_);

  // Create the directory if it doesn't exist
  if (!QFile::exists(directory_)) {
    QDir dir;
    if (!dir.mkpath(directory_)) {
    }
  }

  const QString connection_id =
      QString("%1_thread_%2").arg(connection_id_).arg(
        reinterpret_cast<quint64>(QThread::currentThread()));

  // Try to find an existing connection for this thread
  QSqlDatabase db = QSqlDatabase::database(connection_id);
  if (db.isOpen()) {
    return db;
  }

  db = QSqlDatabase::addDatabase("QSQLITE", connection_id);

  if (!injected_database_name_.isNull())
    db.setDatabaseName(injected_database_name_);
  else
    db.setDatabaseName(directory_ + "/" + kDatabaseFilename);

  if (!db.open()) {
    app_->AddError("Database: " + db.lastError().text());
    return db;
  }

  // Find Sqlite3 functions in the Qt plugin.
  StaticInit();

  QSqlQuery set_fts_tokenizer("SELECT fts3_tokenizer(:name, :pointer)", db);
  set_fts_tokenizer.bindValue(":name", "unicode");
  set_fts_tokenizer.bindValue(":pointer", QByteArray(
      reinterpret_cast<const char*>(&sFTSTokenizer), sizeof(&sFTSTokenizer)));
  if (!set_fts_tokenizer.exec()) {
    qLog(Warning) << "Couldn't register FTS3 tokenizer";
  }

  if (db.tables().count() == 0) {
    // Set up initial schema
    qLog(Info) << "Creating initial database schema";
    UpdateDatabaseSchema(0, db);
  }

  // Attach external databases
  foreach (const QString& key, attached_databases_.keys()) {
    QString filename = attached_databases_[key].filename_;

    if (!injected_database_name_.isNull())
      filename = injected_database_name_;

    // Attach the db
    QSqlQuery q("ATTACH DATABASE :filename AS :alias", db);
    q.bindValue(":filename", filename);
    q.bindValue(":alias", key);
    if (!q.exec()) {
      qFatal("Couldn't attach external database '%s'", key.toAscii().constData());
    }
  }

  if(startup_schema_version_ == -1) {
    UpdateMainSchema(&db);
  }

  // We might have to initialise the schema in some attached databases now, if
  // they were deleted and don't match up with the main schema version.
  foreach (const QString& key, attached_databases_.keys()) {
    // Find out if there are any tables in this database
    QSqlQuery q(QString("SELECT ROWID FROM %1.sqlite_master"
                        " WHERE type='table'").arg(key), db);
    if (!q.exec() || !q.next()) {
      ScopedTransaction t(&db);
      ExecFromFile(attached_databases_[key].schema_, db, 0);
      t.Commit();
    }
  }

  return db;
}

void Database::UpdateMainSchema(QSqlDatabase* db) {
  // Get the database's schema version
  QSqlQuery q("SELECT version FROM schema_version", *db);
  int schema_version = 0;
  if (q.next())
    schema_version = q.value(0).toInt();

  startup_schema_version_ = schema_version;

  if (schema_version > kSchemaVersion) {
    qLog(Warning) << "The database schema (version" << schema_version << ") is newer than I was expecting";
    return;
  }
  if (schema_version < kSchemaVersion) {
    // Update the schema
    for (int v=schema_version+1 ; v<= kSchemaVersion ; ++v) {
      UpdateDatabaseSchema(v, *db);
    }
  }
}

void Database::RecreateAttachedDb(const QString& database_name) {
  if (!attached_databases_.contains(database_name)) {
    qLog(Warning) << "Attached database does not exist:" << database_name;
    return;
  }

  const QString filename = attached_databases_[database_name].filename_;

  QMutexLocker l(&mutex_);
  {
    QSqlDatabase db(Connect());

    QSqlQuery q("DETACH DATABASE :alias", db);
    q.bindValue(":alias", database_name);
    if (!q.exec()) {
      qLog(Warning) << "Failed to detach database" << database_name;
      return;
    }

    if (!QFile::remove(filename)) {
      qLog(Warning) << "Failed to remove file" << filename;
    }
  }

  // We can't just re-attach the database now because it needs to be done for
  // each thread.  Close all the database connections, so each thread will
  // re-attach it when they next connect.
  foreach (const QString& name, QSqlDatabase::connectionNames()) {
    QSqlDatabase::removeDatabase(name);
  }
}

void Database::UpdateDatabaseSchema(int version, QSqlDatabase &db) {
  QString filename;
  if (version == 0)
    filename = ":/schema/schema.sql";
  else
    filename = QString(":/schema/schema-%1.sql").arg(version);
  
  ScopedTransaction t(&db);
  
  if (version == 31) {
    // This version used to do a bad job of converting filenames in the songs
    // table to file:// URLs.  Now we do it properly here instead.
    
    UrlEncodeFilenameColumn("songs", db);
    UrlEncodeFilenameColumn("playlist_items", db);

    foreach (const QString& table, db.tables()) {
      if (table.startsWith("device_") && table.endsWith("_songs")) {
        UrlEncodeFilenameColumn(table, db);
      }
    }
  }
  
  qLog(Debug) << "Applying database schema update" << version
              << "from" << filename;
  ExecFromFile(filename, db, version - 1);
  t.Commit();
}

void Database::UrlEncodeFilenameColumn(const QString& table, QSqlDatabase& db) {
  QSqlQuery select(QString("SELECT ROWID, filename FROM %1").arg(table), db);
  QSqlQuery update(QString("UPDATE %1 SET filename=:filename WHERE ROWID=:id").arg(table), db);
  
  select.exec();
  if (CheckErrors(select)) return;
  while (select.next()) {
    const int rowid = select.value(0).toInt();
    const QString filename = select.value(1).toString();
    
    if (filename.isEmpty() || filename.contains("://")) {
      continue;
    }
    
    const QUrl url = QUrl::fromLocalFile(filename);
    
    update.bindValue(":filename", url.toEncoded());
    update.bindValue(":id", rowid);
    update.exec();
    CheckErrors(update);
  }
}

void Database::ExecFromFile(const QString &filename, QSqlDatabase &db,
                            int schema_version) {
  // Open and read the database schema
  QFile schema_file(filename);
  if (!schema_file.open(QIODevice::ReadOnly))
    qFatal("Couldn't open schema file %s", filename.toUtf8().constData());
  ExecCommands(QString::fromUtf8(schema_file.readAll()), db, schema_version);
}

void Database::ExecCommands(const QString& schema, QSqlDatabase& db,
                            int schema_version) {
  // Run each command
  QStringList commands(schema.split(";\n\n"));

  // We don't want this list to reflect possible DB schema changes
  // so we initialize it before executing any statements.
  QStringList tables = SongsTables(db, schema_version);

  foreach (const QString& command, commands) {
    // There are now lots of "songs" tables that need to have the same schema:
    // songs, magnatune_songs, and device_*_songs.  We allow a magic value
    // in the schema files to update all songs tables at once.
    if (command.contains(kMagicAllSongsTables)) {
      foreach (const QString& table, tables) {
        qLog(Info) << "Updating" << table << "for" << kMagicAllSongsTables;
        QString new_command(command);
        new_command.replace(kMagicAllSongsTables, table);
        QSqlQuery query(db.exec(new_command));
        if (CheckErrors(query))
          qFatal("Unable to update music library database");
      }
    } else {
      QSqlQuery query(db.exec(command));
      if (CheckErrors(query))
        qFatal("Unable to update music library database");
    }
  }
}

QStringList Database::SongsTables(QSqlDatabase& db, int schema_version) const {
  QStringList ret;

  // look for the tables in the main db
  foreach (const QString& table, db.tables()) {
    if (table == "songs" || table.endsWith("_songs"))
      ret << table;
  }

  // look for the tables in attached dbs
  foreach (const QString& key, attached_databases_.keys()) {
    QSqlQuery q(QString("SELECT NAME FROM %1.sqlite_master"
                        " WHERE type='table' AND name='songs' OR name LIKE '%songs'").arg(key), db);
    if (q.exec()) {
      while(q.next()) {
        QString tab_name = key + "." + q.value(0).toString();
        ret << tab_name;
      }
    }
  }

  if (schema_version > 29) {
    // The playlist_items table became a songs table in version 29.
    ret << "playlist_items";
  }

  return ret;
}

bool Database::CheckErrors(const QSqlQuery& query) {
  QSqlError last_error = query.lastError();
  if (last_error.isValid()) {
    qLog(Error) << "db error: " << last_error;
    qLog(Error) << "faulty query: " << query.lastQuery();
    qLog(Error) << "bound values: " << query.boundValues();

    app_->AddError("LibraryBackend: " + last_error.text());
    return true;
  }

  return false;
}

bool Database::IntegrityCheck(QSqlDatabase db) {
  qLog(Debug) << "Starting database integrity check";
  int task_id = app_->task_manager()->StartTask(tr("Integrity check"));

  bool ok = false;
  bool error_reported = false;
  // Ask for 10 error messages at most.
  QSqlQuery q(QString("PRAGMA integrity_check(10)"), db);
  while (q.next()) {
    QString message = q.value(0).toString();

    // If no errors are found, a single row with the value "ok" is returned
    if (message == "ok") {
      ok = true;
      break;
    } else {
      if (!error_reported) {
        app_->AddError(tr("Database corruption detected. Please read "
            "https://code.google.com/p/clementine-player/wiki/DatabaseCorruption "
            "for instructions on how to recover your database"));
      }
      app_->AddError("Database: " + message);
      error_reported = true;
    }
  }

  app_->task_manager()->SetTaskFinished(task_id);

  return ok;
}

void Database::DoBackup() {
  QSqlDatabase db(this->Connect());

  // Before we overwrite anything, make sure the database is not corrupt
  QMutexLocker l(&mutex_);
  const bool ok = IntegrityCheck(db);

  if (ok) {
    BackupFile(db.databaseName());
  }
}

bool Database::OpenDatabase(const QString& filename, sqlite3** connection) const {
  int ret = _sqlite3_open(filename.toUtf8(), connection);
  if (ret != 0) {
    if (*connection) {
      const char* error_message = _sqlite3_errmsg(*connection);
      qLog(Error) << "Failed to open database for backup:"
                  << filename
                  << error_message;
    } else {
      qLog(Error) << "Failed to open database for backup:"
                  << filename;
    }
    return false;
  }
  return true;
}

void Database::BackupFile(const QString& filename) {
  qLog(Debug) << "Starting database backup";
  QString dest_filename = QString("%1.bak").arg(filename);
  const int task_id = app_->task_manager()->StartTask(tr("Backing up database"));

  sqlite3* source_connection = NULL;
  sqlite3* dest_connection = NULL;

  BOOST_SCOPE_EXIT((source_connection)(dest_connection)(task_id)(app_)) {
    // Harmless to call sqlite3_close() with a NULL pointer.
    _sqlite3_close(source_connection);
    _sqlite3_close(dest_connection);
    app_->task_manager()->SetTaskFinished(task_id);
  } BOOST_SCOPE_EXIT_END

  bool success = OpenDatabase(filename, &source_connection);
  if (!success) {
    return;
  }

  success = OpenDatabase(dest_filename, &dest_connection);
  if (!success) {
    return;
  }

  sqlite3_backup* backup = _sqlite3_backup_init(
      dest_connection, "main",
      source_connection, "main");
  if (!backup) {
    const char* error_message = _sqlite3_errmsg(dest_connection);
    qLog(Error) << "Failed to start database backup:" << error_message;
    return;
  }

  int ret = SQLITE_OK;
  do {
    ret = _sqlite3_backup_step(backup, 16);
    const int page_count = _sqlite3_backup_pagecount(backup);
    app_->task_manager()->SetTaskProgress(
        task_id, page_count - _sqlite3_backup_remaining(backup), page_count);
  } while (ret == SQLITE_OK);

  if (ret != SQLITE_DONE) {
    qLog(Error) << "Database backup failed";
  }

  _sqlite3_backup_finish(backup);
}
