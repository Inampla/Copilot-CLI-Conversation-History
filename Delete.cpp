#include <iostream>
#include <Windows.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>

using namespace std;

struct Workspace {
    string session_folder_name;
    string id;
    string cwd;
    string summary;
    string summary_count;
    string created_at;
    string updated_at;
};

struct DeleteResult {
    bool ok;
    string message;
};

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

    filesystem::path eventsFilePath = targetPath / "events.jsonl";
    if (!filesystem::exists(eventsFilePath, ec) || !filesystem::is_regular_file(eventsFilePath, ec))
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

    result.ok = true;
    result.message = "Delete success.";
    return result;
}
