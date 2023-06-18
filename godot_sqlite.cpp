#include "core/core_bind.h"
#include "core/os/os.h"
#include "editor/project_settings_editor.h"
#include "thirdparty/sqlite/sqlite3.h"

#include "godot_sqlite.h"

#define _GNU_SOURCE

Array fast_parse_row(sqlite3_stmt *stmt) {
	Array result;

	// Get column count
	const int col_count = sqlite3_column_count(stmt);

	// Fetch all column
	for (int i = 0; i < col_count; i++) {
		// Value
		const int col_type = sqlite3_column_type(stmt, i);
		Variant value;

		// Get column value
		switch (col_type) {
			case SQLITE_INTEGER:
				value = Variant(sqlite3_column_int(stmt, i));
				break;

			case SQLITE_FLOAT:
				value = Variant(sqlite3_column_double(stmt, i));
				break;

			case SQLITE_TEXT: {
				int size = sqlite3_column_bytes(stmt, i);
				String str =
						String::utf8((const char *)sqlite3_column_text(stmt, i), size);
				value = Variant(str);
				break;
			}
			case SQLITE_BLOB: {
				PackedByteArray arr;
				int size = sqlite3_column_bytes(stmt, i);
				arr.resize(size);
				memcpy(arr.ptrw(), sqlite3_column_blob(stmt, i), size);
				value = Variant(arr);
				break;
			}
			case SQLITE_NULL: {
				// Nothing to do.
			} break;
			default:
				ERR_PRINT("This kind of data is not yet supported: " + itos(col_type));
				break;
		}

		result.push_back(value);
	}

	return result;
}

SQLiteQuery::SQLiteQuery() {}

SQLiteQuery::~SQLiteQuery() { finalize(); }

void SQLiteQuery::init(SQLite *p_db, const String &p_query) {
	db = p_db;
	query = p_query;
	stmt = nullptr;
}

bool SQLiteQuery::is_ready() const { return stmt != nullptr; }

String SQLiteQuery::get_last_error_message() const {
	ERR_FAIL_COND_V(db == nullptr, "Database is undefined.");
	return db->get_last_error_message();
}

Array SQLiteQuery::get_columns() {
	if (is_ready() == false) {
		ERR_FAIL_COND_V(prepare() == false, Array());
	}

	// At this point stmt can't be null.
	CRASH_COND(stmt == nullptr);

	Array res;
	const int col_count = sqlite3_column_count(stmt);
	res.resize(col_count);

	// Fetch all column
	for (int i = 0; i < col_count; i++) {
		// Key name
		const char *col_name = sqlite3_column_name(stmt, i);
		res[i] = String(col_name);
	}

	return res;
}

bool SQLiteQuery::prepare() {
	ERR_FAIL_COND_V(stmt != nullptr, false);
	ERR_FAIL_COND_V(db == nullptr, false);
	ERR_FAIL_COND_V(query == "", false);

	// Prepare the statement
	int result = sqlite3_prepare_v2(db->get_handler(), query.utf8().ptr(), -1,
			&stmt, nullptr);

	// Cannot prepare query!
	ERR_FAIL_COND_V_MSG(result != SQLITE_OK, false,
			"SQL Error: " + db->get_last_error_message());

	return true;
}

void SQLiteQuery::finalize() {
	if (stmt) {
		sqlite3_finalize(stmt);
		stmt = nullptr;
	}
}

void SQLiteQuery::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_last_error_message"),
			&SQLiteQuery::get_last_error_message);
	ClassDB::bind_method(D_METHOD("execute", "arguments"), &SQLiteQuery::execute,
			DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("batch_execute", "rows"),
			&SQLiteQuery::batch_execute);
	ClassDB::bind_method(D_METHOD("get_columns"), &SQLiteQuery::get_columns);
}

SQLite::SQLite() {
	db = nullptr;
	memory_read = false;
}

bool SQLite::open_in_memory() {
	int result = sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
	ERR_FAIL_COND_V_MSG(result != SQLITE_OK, false,
			"Cannot open database in memory, error:" + itos(result));
	return true;
}

void SQLite::close() {
	// Finalize all queries before close the DB.
	// Reverse order because I need to remove the not available queries.
	for (uint32_t i = queries.size(); i > 0; i -= 1) {
		SQLiteQuery *query =
				Object::cast_to<SQLiteQuery>(queries[i - 1]->get_ref());
		if (query != nullptr) {
			query->finalize();
		} else {
			memdelete(queries[i - 1]);
			queries.remove_at(i - 1);
		}
	}

	if (db) {
		// Cannot close database!
		if (sqlite3_close_v2(db) != SQLITE_OK) {
			print_error("Cannot close database: " + get_last_error_message());
		} else {
			db = nullptr;
		}
	}

	if (memory_read) {
		// Close virtual filesystem database
		spmemvfs_close_db(&p_db);
		spmemvfs_env_fini();
		memory_read = false;
	}
}

sqlite3_stmt *SQLite::prepare(const char *query) {
	// Get database pointer
	sqlite3 *dbs = get_handler();

	ERR_FAIL_COND_V_MSG(dbs == nullptr, nullptr,
			"Cannot prepare query! Database is not opened.");

	// Prepare the statement
	sqlite3_stmt *stmt = nullptr;
	int result = sqlite3_prepare_v2(dbs, query, -1, &stmt, nullptr);

	// Cannot prepare query!
	ERR_FAIL_COND_V_MSG(result != SQLITE_OK, nullptr,
			"SQL Error: " + get_last_error_message());
	return stmt;
}

Dictionary SQLite::parse_row(sqlite3_stmt *stmt, int result_type) {
	Dictionary result;

	// Get column count
	int col_count = sqlite3_column_count(stmt);

	// Fetch all column
	for (int i = 0; i < col_count; i++) {
		// Key name
		const char *col_name = sqlite3_column_name(stmt, i);
		String key = String(col_name);

		// Value
		int col_type = sqlite3_column_type(stmt, i);
		Variant value;

		// Get column value
		switch (col_type) {
			case SQLITE_INTEGER:
				value = Variant(sqlite3_column_int(stmt, i));
				break;

			case SQLITE_FLOAT:
				value = Variant(sqlite3_column_double(stmt, i));
				break;

			case SQLITE_TEXT: {
				int size = sqlite3_column_bytes(stmt, i);
				String str =
						String::utf8((const char *)sqlite3_column_text(stmt, i), size);
				value = Variant(str);
				break;
			}
			case SQLITE_BLOB: {
				PackedByteArray arr;
				int size = sqlite3_column_bytes(stmt, i);
				arr.resize(size);
				memcpy((void *)arr.ptr(), sqlite3_column_blob(stmt, i), size);
				value = Variant(arr);
				break;
			}

			default:
				break;
		}

		// Set dictionary value
		if (result_type == RESULT_NUM) {
			result[i] = value;
		} else if (result_type == RESULT_ASSOC) {
			result[key] = value;
		} else {
			result[i] = value;
			result[key] = value;
		}
	}

	return result;
}

String SQLite::get_last_error_message() const {
	return sqlite3_errmsg(get_handler());
}

SQLite::~SQLite() {
	// Close database
	close();
	// Make sure to invalidate all associated queries.
	for (uint32_t i = 0; i < queries.size(); i += 1) {
		SQLiteQuery *query = Object::cast_to<SQLiteQuery>(queries[i]->get_ref());
		if (query != nullptr) {
			query->init(nullptr, "");
		}
	}
}

void SQLite::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "path"), &SQLite::open);
	ClassDB::bind_method(D_METHOD("open_in_memory"), &SQLite::open_in_memory);
	ClassDB::bind_method(D_METHOD("open_buffered", "path", "buffers", "size"),
			&SQLite::open_buffered);

	ClassDB::bind_method(D_METHOD("close"), &SQLite::close);

	ClassDB::bind_method(D_METHOD("create_query", "statement"),
			&SQLite::create_query);
}