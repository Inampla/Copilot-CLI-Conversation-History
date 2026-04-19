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

vector<string> FindFoldersContainingEventsJsonl(const string& sessionStatePath)
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

        filesystem::path eventsFilePath = entry.path() / "events.jsonl";
        if (filesystem::exists(eventsFilePath, ec) && filesystem::is_regular_file(eventsFilePath, ec))
        {
            result.push_back(entry.path().filename().string());
        }

        ec.clear();
    }

    sort(result.begin(), result.end());
    return result;
}

static const string Copilot_Path = "~/.copilot/session-state";

struct Workspace {
    string session_folder_name;
    string id;
    string cwd;
    string summary;
    string summary_count;
    string created_at;
    string updated_at;
};

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
    ifstream fin(workspaceYamlPath);
    if (!fin.is_open())
    {
        return workspace;
    }

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

vector<Workspace> LoadAllWorkspace()
{
    vector<string> copilotSession = FindFoldersContainingEventsJsonl(Copilot_Path);
    vector<Workspace> returnAllWorkspace = ReadAllWorkspaceYamlInSessionOrder(copilotSession);
    return returnAllWorkspace;
}

/*
int main()
{
    vector<string> testSession = FindFoldersContainingEventsJsonl(Copilot_Path);
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
