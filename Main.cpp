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

#include "SessionModel.h"

using namespace std;

enum PageMode {
    PageList = 0,
    PageDetail = 1,
    PageDeleteConfirm = 2
};

enum TuiActionType {
    TuiNone = 0,
    TuiMoveUp = 1,
    TuiMoveDown = 2,
    TuiOpenDetail = 3,
    TuiBack = 4,
    TuiOpenDeleteDialog = 5,
    TuiDeleteSelectLeft = 6,
    TuiDeleteSelectRight = 7,
    TuiCloseDeleteDialog = 8,
    TuiConfirmDelete = 9,
    TuiQuit = 10
};

struct TuiAction {
    TuiActionType type;
};

TuiAction ShowTuiAndWait(
    const vector<Workspace>& allWorkspace,
    size_t& selectedIndex,
    size_t& topVisibleIndex,
    string& statusText,
    PageMode& currentPage,
    bool& showDeleteDialog,
    bool& deleteYesSelected
);

vector<Workspace> LoadAllWorkspace();
DeleteResult SafeDeleteWorkspaceSession(const Workspace& workspace);

/*
int main()
{
    vector<Workspace> AllWorkspace = LoadAllWorkspace();
    for (size_t i = 0; i < AllWorkspace.size(); i++)
    {
        cout << "id: " << AllWorkspace[i].id << endl;
        cout << "summary: " << AllWorkspace[i].summary << endl;
        cout << "cwd: " << AllWorkspace[i].cwd << endl;
        cout << "created_at: " << AllWorkspace[i].created_at << endl;
        cout << "updated_at: " << AllWorkspace[i].updated_at << endl;
        cout << endl;
    }
    return 0;
}
*/

int main()
{
    bool showDeleteDialog = false;
    bool deleteYesSelected = false;
    vector<Workspace> allWorkspace = LoadAllWorkspace();

    size_t selectedIndex = 0;
    size_t topVisibleIndex = 0;

    PageMode currentPage = PageList;
    string statusText = "Loaded " + to_string(allWorkspace.size()) + " sessions.";

    while (true)
    {
        TuiAction action = ShowTuiAndWait(
            allWorkspace,
            selectedIndex,
            topVisibleIndex,
            statusText,
            currentPage,
            showDeleteDialog,
            deleteYesSelected
        );

        if (action.type == TuiQuit)
        {
            break;
        }

        if (action.type == TuiConfirmDelete)
        {
            if (allWorkspace.empty() || selectedIndex >= allWorkspace.size())
            {
                statusText = "Delete failed: invalid selection.";
            }
            else
            {
                Workspace targetWorkspace = allWorkspace[selectedIndex];
                DeleteResult deleteResult = SafeDeleteWorkspaceSession(targetWorkspace);

                if (deleteResult.ok)
                {
                    allWorkspace = LoadAllWorkspace();
                    currentPage = PageList;

                    if (allWorkspace.empty())
                    {
                        selectedIndex = 0;
                        topVisibleIndex = 0;
                    }
                    else
                    {
                        if (selectedIndex >= allWorkspace.size())
                        {
                            selectedIndex = allWorkspace.size() - 1;
                        }

                        if (selectedIndex < topVisibleIndex)
                        {
                            topVisibleIndex = selectedIndex;
                        }

                        if (topVisibleIndex >= allWorkspace.size())
                        {
                            topVisibleIndex = selectedIndex;
                        }
                    }

                    statusText = deleteResult.message + " Loaded " + to_string(allWorkspace.size()) + " sessions.";
                }
                else
                {
                    statusText = "Delete failed: " + deleteResult.message;
                }
            }
        }
    }

    return 0;
}
