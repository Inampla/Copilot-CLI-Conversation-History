#include <iostream>
#include <Windows.h>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <sqlite3.h>

#include "SessionModel.h"

using namespace std;

static filesystem::path ExpandUserPath(const string& rawPath)
{
    if (rawPath.empty())
    {
        return filesystem::path();
    }

    if (rawPath[0] != '~')
    {
        return filesystem::path(rawPath);
    }

    char* userProfile = nullptr;
    size_t userProfileLen = 0;
    errno_t err = _dupenv_s(&userProfile, &userProfileLen, "USERPROFILE");
    if (err != 0 || userProfile == nullptr)
    {
        return filesystem::path(rawPath);
    }

    string expanded = userProfile;
    free(userProfile);

    if (rawPath.size() >= 2)
    {
        expanded += rawPath.substr(1);
    }

    return filesystem::path(expanded);
}

static string WideToUtf8(const wstring& value)
{
    if (value.empty())
    {
        return "";
    }

    int requiredSize = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (requiredSize <= 0)
    {
        return "";
    }

    string result(static_cast<size_t>(requiredSize), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        result.data(),
        requiredSize,
        nullptr,
        nullptr
    );

    return result;
}

static bool IsSafeSingleFolderName(const string& folderName)
{
    if (folderName.empty())
    {
        return false;
    }

    if (folderName == "." || folderName == "..")
    {
        return false;
    }

    if (folderName == "_deleted")
    {
        return false;
    }

    for (size_t i = 0; i < folderName.size(); i++)
    {
        char c = folderName[i];
        if (c == '/' || c == '\\' || c == ':')
        {
            return false;
        }
    }

    return true;
}

static vector<string> BuildSessionIdCandidates(const Workspace& workspace)
{
    vector<string> ids;

    if (!workspace.id.empty())
    {
        ids.push_back(workspace.id);
    }

    if (!workspace.session_folder_name.empty() && workspace.session_folder_name != workspace.id)
    {
        ids.push_back(workspace.session_folder_name);
    }

    return ids;
}

static string QuoteSqlIdentifier(const string& identifier)
{
    string quoted = "\"";
    for (char c : identifier)
    {
        if (c == '"')
        {
            quoted += "\"\"";
        }
        else
        {
            quoted += c;
        }
    }
    quoted += "\"";
    return quoted;
}

static string ToLowerAscii(string value)
{
    transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c)
        {
            return static_cast<char>(tolower(c));
        }
    );

    return value;
}

static bool StartsWith(const string& value, const string& prefix)
{
    return value.size() >= prefix.size() &&
        value.compare(0, prefix.size(), prefix) == 0;
}

static bool IsSkippableSessionStoreTable(const string& tableName, const string& createSql)
{
    string lowerName = ToLowerAscii(tableName);
    string lowerSql = ToLowerAscii(createSql);

    if (StartsWith(lowerName, "sqlite_"))
    {
        return true;
    }

    if (lowerSql.find("create virtual table") != string::npos ||
        lowerSql.find(" using fts") != string::npos)
    {
        return true;
    }

    return false;
}

static bool ExecSql(sqlite3* db, const char* sql, string& errorMessage)
{
    char* rawError = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &rawError);
    if (rc == SQLITE_OK)
    {
        return true;
    }

    if (rawError != nullptr)
    {
        errorMessage = rawError;
        sqlite3_free(rawError);
    }
    else
    {
        errorMessage = sqlite3_errmsg(db);
    }

    return false;
}

static bool ListSessionStoreTables(sqlite3* db, vector<string>& tables, string& errorMessage)
{
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT name, COALESCE(sql, '') FROM sqlite_master WHERE type = 'table' ORDER BY name",
        -1,
        &stmt,
        nullptr
    );

    if (rc != SQLITE_OK)
    {
        errorMessage = sqlite3_errmsg(db);
        return false;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        const unsigned char* rawName = sqlite3_column_text(stmt, 0);
        const unsigned char* rawSql = sqlite3_column_text(stmt, 1);
        if (rawName != nullptr)
        {
            string tableName = reinterpret_cast<const char*>(rawName);
            string createSql;
            if (rawSql != nullptr)
            {
                createSql = reinterpret_cast<const char*>(rawSql);
            }

            if (!IsSkippableSessionStoreTable(tableName, createSql))
            {
                tables.push_back(tableName);
            }
        }
    }

    if (rc != SQLITE_DONE)
    {
        errorMessage = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

static bool TableHasColumn(sqlite3* db, const string& tableName, const string& columnName, bool& hasColumn, string& errorMessage)
{
    hasColumn = false;
    string sql = "PRAGMA table_info(" + QuoteSqlIdentifier(tableName) + ")";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        errorMessage = sqlite3_errmsg(db);
        return false;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        const unsigned char* rawName = sqlite3_column_text(stmt, 1);
        if (rawName != nullptr && columnName == reinterpret_cast<const char*>(rawName))
        {
            hasColumn = true;
            break;
        }
    }

    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    {
        errorMessage = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

static bool DeleteRowsByColumn(
    sqlite3* db,
    const string& tableName,
    const string& columnName,
    const vector<string>& values,
    int& deletedRows,
    string& errorMessage
)
{
    string sql = "DELETE FROM " + QuoteSqlIdentifier(tableName) + " WHERE " + QuoteSqlIdentifier(columnName) + " = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        errorMessage = sqlite3_errmsg(db);
        return false;
    }

    for (const string& value : values)
    {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        rc = sqlite3_bind_text(stmt, 1, value.c_str(), -1, SQLITE_TRANSIENT);
        if (rc != SQLITE_OK)
        {
            errorMessage = sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            return false;
        }

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE)
        {
            errorMessage = sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            return false;
        }

        deletedRows += sqlite3_changes(db);
    }

    sqlite3_finalize(stmt);
    return true;
}

static bool DeleteSessionStoreRows(const filesystem::path& sessionStorePath, const vector<string>& ids, int& deletedRows, string& errorMessage)
{
    deletedRows = 0;
    if (ids.empty())
    {
        return true;
    }

    error_code ec;
    if (!filesystem::exists(sessionStorePath, ec) || !filesystem::is_regular_file(sessionStorePath, ec))
    {
        return true;
    }

    string databasePath = WideToUtf8(sessionStorePath.wstring());
    if (databasePath.empty())
    {
        errorMessage = "Failed to encode session-store path.";
        return false;
    }

    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(
        databasePath.c_str(),
        &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );

    if (rc != SQLITE_OK)
    {
        errorMessage = db == nullptr ? "Failed to open session-store.db." : sqlite3_errmsg(db);
        if (db != nullptr)
        {
            sqlite3_close(db);
        }
        return false;
    }

    sqlite3_busy_timeout(db, 3000);

    if (!ExecSql(db, "PRAGMA foreign_keys = ON", errorMessage) ||
        !ExecSql(db, "BEGIN IMMEDIATE", errorMessage))
    {
        sqlite3_close(db);
        return false;
    }

    vector<string> tables;
    if (!ListSessionStoreTables(db, tables, errorMessage))
    {
        ExecSql(db, "ROLLBACK", errorMessage);
        sqlite3_close(db);
        return false;
    }

    for (const string& tableName : tables)
    {
        if (tableName == "sessions")
        {
            continue;
        }

        bool hasSessionId = false;
        if (!TableHasColumn(db, tableName, "session_id", hasSessionId, errorMessage))
        {
            ExecSql(db, "ROLLBACK", errorMessage);
            sqlite3_close(db);
            return false;
        }

        if (hasSessionId &&
            !DeleteRowsByColumn(db, tableName, "session_id", ids, deletedRows, errorMessage))
        {
            ExecSql(db, "ROLLBACK", errorMessage);
            sqlite3_close(db);
            return false;
        }
    }

    bool hasSessionsId = false;
    if (!TableHasColumn(db, "sessions", "id", hasSessionsId, errorMessage))
    {
        ExecSql(db, "ROLLBACK", errorMessage);
        sqlite3_close(db);
        return false;
    }

    if (hasSessionsId &&
        !DeleteRowsByColumn(db, "sessions", "id", ids, deletedRows, errorMessage))
    {
        ExecSql(db, "ROLLBACK", errorMessage);
        sqlite3_close(db);
        return false;
    }

    if (!ExecSql(db, "COMMIT", errorMessage))
    {
        ExecSql(db, "ROLLBACK", errorMessage);
        sqlite3_close(db);
        return false;
    }

    sqlite3_close(db);
    return true;
}

static string BuildTimestamp()
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    char buffer[64];
    sprintf_s(
        buffer,
        "%04d%02d%02d_%02d%02d%02d",
        static_cast<int>(st.wYear),
        static_cast<int>(st.wMonth),
        static_cast<int>(st.wDay),
        static_cast<int>(st.wHour),
        static_cast<int>(st.wMinute),
        static_cast<int>(st.wSecond)
    );

    return string(buffer);
}

static filesystem::path BuildUniqueDeletedPath(
    const filesystem::path& deletedRoot,
    const string& sessionFolderName,
    const string& timestamp
)
{
    error_code ec;

    for (size_t i = 0; i < 1000; i++)
    {
        string candidateName = sessionFolderName + "__deleted_" + timestamp;
        if (i > 0)
        {
            candidateName += "_" + to_string(i);
        }

        filesystem::path candidatePath = deletedRoot / candidateName;
        if (!filesystem::exists(candidatePath, ec))
        {
            return candidatePath;
        }

        ec.clear();
    }

    return deletedRoot / (sessionFolderName + "__deleted_" + timestamp + "_overflow");
}

DeleteResult SafeDeleteWorkspaceSession(const Workspace& workspace)
{
    DeleteResult result;
    result.ok = false;
    result.message = "";

    if (!IsSafeSingleFolderName(workspace.session_folder_name))
    {
        result.message = "Invalid session folder name.";
        return result;
    }

    filesystem::path sessionRoot = ExpandUserPath("~/.copilot/session-state");
    error_code ec;

    if (!filesystem::exists(sessionRoot, ec) || !filesystem::is_directory(sessionRoot, ec))
    {
        result.message = "Session-state folder not found.";
        return result;
    }

    filesystem::path targetPath = sessionRoot / workspace.session_folder_name;
    filesystem::path targetParent = targetPath.parent_path().lexically_normal();
    filesystem::path rootNormalized = sessionRoot.lexically_normal();

    if (targetParent != rootNormalized)
    {
        result.message = "Target path is outside session-state.";
        return result;
    }

    if (!filesystem::exists(targetPath, ec) || !filesystem::is_directory(targetPath, ec))
    {
        result.message = "Target session folder not found.";
        return result;
    }

    filesystem::path workspaceYamlPath = targetPath / "workspace.yaml";
    if (!filesystem::exists(workspaceYamlPath, ec) || !filesystem::is_regular_file(workspaceYamlPath, ec))
    {
        result.message = "Target folder is not a valid session folder.";
        return result;
    }

    filesystem::path deletedRoot = sessionRoot / "_deleted";
    filesystem::create_directories(deletedRoot, ec);
    if (ec)
    {
        result.message = "Failed to create _deleted folder.";
        return result;
    }

    string timestamp = BuildTimestamp();
    filesystem::path destinationPath = BuildUniqueDeletedPath(
        deletedRoot,
        workspace.session_folder_name,
        timestamp
    );

    filesystem::rename(targetPath, destinationPath, ec);
    if (ec)
    {
        result.message = "Failed to move session folder.";
        return result;
    }

    vector<string> sessionIds = BuildSessionIdCandidates(workspace);
    filesystem::path sessionStorePath = sessionRoot.parent_path() / "session-store.db";
    int deletedStoreRows = 0;
    string storeError;
    bool storeCleaned = DeleteSessionStoreRows(sessionStorePath, sessionIds, deletedStoreRows, storeError);

    result.ok = true;
    if (!storeCleaned)
    {
        result.message = "Session folder moved, but session-store cleanup failed: " + storeError;
    }
    else if (deletedStoreRows > 0)
    {
        result.message = "Delete success. Cleaned " + to_string(deletedStoreRows) + " session-store rows.";
    }
    else
    {
        result.message = "Delete success.";
    }

    return result;
}
