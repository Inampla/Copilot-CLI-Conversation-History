#include <iostream>
#include <cmath>
#include <Windows.h>
#include <xmemory>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <fstream>
#include <cctype>
#include <set>
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

static bool IsDeletedFolderName(const string& folderName)
{
    return folderName == "_deleted";
}

vector<string> FindSessionFolders(const string& sessionStatePath)
{
    vector<string> result;
    filesystem::path rootPath = ExpandUserPath(sessionStatePath);
    error_code ec;

    if (!filesystem::exists(rootPath, ec) || !filesystem::is_directory(rootPath, ec))
    {
        return result;
    }

    filesystem::directory_iterator it(rootPath, ec);
    filesystem::directory_iterator end;
    if (ec)
    {
        return result;
    }

    for (; it != end; it++)
    {
        if (ec)
        {
            ec.clear();
            continue;
        }

        const filesystem::directory_entry& entry = *it;
        if (!entry.is_directory(ec))
        {
            ec.clear();
            continue;
        }

        string folderName = entry.path().filename().string();
        if (IsDeletedFolderName(folderName))
        {
            continue;
        }

        filesystem::path workspaceYamlPath = entry.path() / "workspace.yaml";
        if (filesystem::exists(workspaceYamlPath, ec) && filesystem::is_regular_file(workspaceYamlPath, ec))
        {
            result.push_back(folderName);
        }

        ec.clear();
    }

    sort(result.begin(), result.end());
    return result;
}

static const string Copilot_Path = "~/.copilot/session-state";

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

static bool SessionStoreHasTable(sqlite3* db, const string& tableName)
{
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(
        db,
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? LIMIT 1",
        -1,
        &stmt,
        nullptr
    );

    if (rc != SQLITE_OK)
    {
        return false;
    }

    sqlite3_bind_text(stmt, 1, tableName.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    bool exists = rc == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

static set<string> LoadSessionIdsWithTurns()
{
    set<string> sessionIds;
    filesystem::path sessionStorePath = ExpandUserPath(Copilot_Path).parent_path() / "session-store.db";
    error_code ec;

    if (!filesystem::exists(sessionStorePath, ec) || !filesystem::is_regular_file(sessionStorePath, ec))
    {
        return sessionIds;
    }

    string databasePath = WideToUtf8(sessionStorePath.wstring());
    if (databasePath.empty())
    {
        return sessionIds;
    }

    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(
        databasePath.c_str(),
        &db,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
        nullptr
    );

    if (rc != SQLITE_OK)
    {
        if (db != nullptr)
        {
            sqlite3_close(db);
        }
        return sessionIds;
    }

    sqlite3_busy_timeout(db, 3000);

    if (!SessionStoreHasTable(db, "turns"))
    {
        sqlite3_close(db);
        return sessionIds;
    }

    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(
        db,
        "SELECT session_id FROM turns GROUP BY session_id",
        -1,
        &stmt,
        nullptr
    );

    if (rc != SQLITE_OK)
    {
        sqlite3_close(db);
        return sessionIds;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        const unsigned char* rawSessionId = sqlite3_column_text(stmt, 0);
        if (rawSessionId != nullptr)
        {
            sessionIds.insert(reinterpret_cast<const char*>(rawSessionId));
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return sessionIds;
}

static bool SetContainsSessionId(const set<string>& sessionIds, const Workspace& workspace)
{
    if (!workspace.id.empty() && sessionIds.find(workspace.id) != sessionIds.end())
    {
        return true;
    }

    if (!workspace.session_folder_name.empty() &&
        sessionIds.find(workspace.session_folder_name) != sessionIds.end())
    {
        return true;
    }

    return false;
}

static string Trim(const string& s)
{
    size_t left = 0;
    size_t right = s.size();

    while (left < right && isspace(static_cast<unsigned char>(s[left])))
    {
        left++;
    }

    while (right > left && isspace(static_cast<unsigned char>(s[right - 1])))
    {
        right--;
    }

    return s.substr(left, right - left);
}

static string RemoveOuterQuotes(const string& s)
{
    if (s.size() >= 2)
    {
        if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))
        {
            return s.substr(1, s.size() - 2);
        }
    }

    return s;
}

static Workspace ReadOneWorkspaceYaml(const string& sessionName)
{
    Workspace workspace;
    workspace.session_folder_name = sessionName;
    workspace.id = sessionName;

    filesystem::path workspaceYamlPath = ExpandUserPath(Copilot_Path) / sessionName / "workspace.yaml";
    filesystem::path eventsFilePath = ExpandUserPath(Copilot_Path) / sessionName / "events.jsonl";
    error_code ec;
    workspace.has_events_jsonl =
        filesystem::exists(eventsFilePath, ec) && filesystem::is_regular_file(eventsFilePath, ec);
    if (workspace.has_events_jsonl)
    {
        uintmax_t eventFileSize = filesystem::file_size(eventsFilePath, ec);
        workspace.has_conversation_data = !ec && eventFileSize > 0;
    }

    ifstream fin(workspaceYamlPath);
    if (!fin.is_open())
    {
        return workspace;
    }

    string workspaceName;
    string line;
    while (getline(fin, line))
    {
        string trimmedLine = Trim(line);

        if (trimmedLine.empty())
        {
            continue;
        }

        if (trimmedLine[0] == '#')
        {
            continue;
        }

        size_t pos = trimmedLine.find(':');
        if (pos == string::npos)
        {
            continue;
        }

        string key = Trim(trimmedLine.substr(0, pos));
        string value = Trim(trimmedLine.substr(pos + 1));
        value = RemoveOuterQuotes(value);

        if (key == "id")
        {
            workspace.id = value;
        }
        else if (key == "cwd")
        {
            workspace.cwd = value;
        }
        else if (key == "repository")
        {
            workspace.repository = value;
        }
        else if (key == "branch")
        {
            workspace.branch = value;
        }
        else if (key == "client_name")
        {
            workspace.client_name = value;
        }
        else if (key == "name")
        {
            workspaceName = value;
        }
        else if (key == "summary")
        {
            workspace.summary = value;
        }
        else if (key == "summary_count")
        {
            workspace.summary_count = value;
        }
        else if (key == "created_at")
        {
            workspace.created_at = value;
        }
        else if (key == "updated_at")
        {
            workspace.updated_at = value;
        }
    }

    if (!workspaceName.empty())
    {
        workspace.summary = workspaceName;
    }

    return workspace;
}

static vector<Workspace> ReadAllWorkspaceYamlInSessionOrder(const vector<string>& copilotSession)
{
    vector<Workspace> result;
    for (size_t i = 0; i < copilotSession.size(); i++)
    {
        Workspace workspace = ReadOneWorkspaceYaml(copilotSession[i]);
        result.push_back(workspace);
    }

    return result;
}

static bool WorkspaceUpdatedLater(const Workspace& left, const Workspace& right)
{
    if (left.updated_at != right.updated_at)
    {
        return left.updated_at > right.updated_at;
    }

    if (left.created_at != right.created_at)
    {
        return left.created_at > right.created_at;
    }

    return left.session_folder_name < right.session_folder_name;
}

vector<Workspace> LoadAllWorkspace()
{
    vector<string> copilotSession = FindSessionFolders(Copilot_Path);
    vector<Workspace> allWorkspace = ReadAllWorkspaceYamlInSessionOrder(copilotSession);
    set<string> sessionIdsWithTurns = LoadSessionIdsWithTurns();
    vector<Workspace> returnAllWorkspace;

    for (Workspace workspace : allWorkspace)
    {
        if (SetContainsSessionId(sessionIdsWithTurns, workspace))
        {
            workspace.has_conversation_data = true;
        }

        if (workspace.has_conversation_data)
        {
            returnAllWorkspace.push_back(workspace);
        }
    }

    sort(returnAllWorkspace.begin(), returnAllWorkspace.end(), WorkspaceUpdatedLater);
    return returnAllWorkspace;
}

/*
int main()
{
    vector<string> testSession = FindSessionFolders(Copilot_Path);
    for (size_t i = 0; i < testSession.size(); i++)
    {
        cout << testSession[i] << endl;
    }
    cout << endl;

    vector<Workspace> test = LoadAllWorkspace();
    for (size_t i = 0; i < test.size(); i++)
    {
        cout << "session folder: " << test[i].session_folder_name << endl;
        cout << "id: " << test[i].id << endl;
        cout << "summary: " << test[i].summary << endl;
        cout << "cwd: " << test[i].cwd << endl;
        cout << "created_at: " << test[i].created_at << endl;
        cout << "updated_at: " << test[i].updated_at << endl;
        cout << endl;
    }
}
*/
