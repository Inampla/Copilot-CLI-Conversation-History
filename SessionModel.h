#pragma once

#include <string>

struct Workspace {
    std::string session_folder_name;
    std::string id;
    std::string cwd;
    std::string repository;
    std::string branch;
    std::string client_name;
    std::string summary;
    std::string summary_count;
    std::string created_at;
    std::string updated_at;
    bool has_events_jsonl = false;
    bool has_conversation_data = false;
};

struct DeleteResult {
    bool ok = false;
    std::string message;
};
