#include <string>
#include <vector>
#include <algorithm>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

using namespace std;
using namespace ftxui;

struct Workspace {
    string session_folder_name;
    string id;
    string cwd;
    string summary;
    string summary_count;
    string created_at;
    string updated_at;
};

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

static string GetFieldOrNA(const string& value)
{
    if (value.empty())
    {
        return "N/A";
    }
    return value;
}

static string GetDisplayName(const Workspace& workspace)
{
    if (!workspace.summary.empty())
    {
        return workspace.summary;
    }
    if (!workspace.id.empty())
    {
        return workspace.id;
    }
    return "N/A";
}

static string CutText(const string& textValue, size_t maxWidth)
{
    if (textValue.size() <= maxWidth)
    {
        return textValue;
    }
    if (maxWidth <= 3)
    {
        return textValue.substr(0, maxWidth);
    }
    return textValue.substr(0, maxWidth - 3) + "...";
}

static string PadRight(const string& textValue, size_t width)
{
    string result = textValue;
    if (result.size() > width)
    {
        result = CutText(result, width);
    }
    if (result.size() < width)
    {
        result += string(width - result.size(), ' ');
    }
    return result;
}

static Element RenderHelpBar(PageMode currentPage, bool showDeleteDialog)
{
    if (showDeleteDialog)
    {
        return text("Left/Right Select   Enter Confirm   Esc Cancel   Q Quit");
    }

    if (currentPage == PageList)
    {
        return text("Up/Down Move   Enter Detail   Del Delete   Q Quit");
    }

    return text("Esc Back   Del Delete   Q Quit");
}

static Element RenderStatusBar(const string& statusText, bool muted)
{
    return hbox({
        text("Status: ") | bold,
        text(statusText) | color(muted ? Color::GrayLight : Color::Yellow)
        });
}

static Element RenderListTable(
    const vector<Workspace>& allWorkspace,
    size_t selectedIndex,
    size_t topVisibleIndex,
    size_t visibleRowCount,
    bool muted
)
{
    vector<Element> lines;

    string header =
        "  "
        + PadRight("No", 4)
        + PadRight("Type", 8)
        + PadRight("Modified", 22)
        + PadRight("Created", 22)
        + "Summary";

    lines.push_back(text(header) | bold);

    if (allWorkspace.empty())
    {
        lines.push_back(separator());
        lines.push_back(text("No session found."));
        return vbox(lines);
    }

    size_t beginIndex = min(topVisibleIndex, allWorkspace.size());
    size_t endIndex = min(beginIndex + visibleRowCount, allWorkspace.size());

    lines.push_back(separator());

    for (size_t i = beginIndex; i < endIndex; i++)
    {
        string row =
            string(i == selectedIndex ? "> " : "  ")
            + PadRight(to_string(i), 4)
            + PadRight("Local", 8)
            + PadRight(GetFieldOrNA(allWorkspace[i].updated_at), 22)
            + PadRight(GetFieldOrNA(allWorkspace[i].created_at), 22)
            + CutText(GetDisplayName(allWorkspace[i]), 64);

        if (i == selectedIndex)
        {
            lines.push_back(text(row) | color(muted ? Color::GrayLight : Color::Cyan) | bold);
        }
        else
        {
            lines.push_back(text(row) | color(muted ? Color::GrayDark : Color::White));
        }
    }

    return vbox(lines);
}

static Element RenderListPage(
    const vector<Workspace>& allWorkspace,
    size_t selectedIndex,
    size_t topVisibleIndex,
    bool muted
)
{
    const size_t visibleRowCount = 12;

    Element listPanel = window(
        text("Session List"),
        RenderListTable(allWorkspace, selectedIndex, topVisibleIndex, visibleRowCount, muted)
    );

    return listPanel;
}

static Element RenderDetailPage(const Workspace* currentWorkspace)
{
    vector<Element> lines;

    if (currentWorkspace == nullptr)
    {
        lines.push_back(text("No session selected."));
        return window(text("Session Detail"), vbox(lines));
    }

    lines.push_back(text("summary: " + GetFieldOrNA(currentWorkspace->summary)));
    lines.push_back(text("id: " + GetFieldOrNA(currentWorkspace->id)));
    lines.push_back(text("summary_count: " + GetFieldOrNA(currentWorkspace->summary_count)));
    lines.push_back(text("created_at: " + GetFieldOrNA(currentWorkspace->created_at)));
    lines.push_back(text("updated_at: " + GetFieldOrNA(currentWorkspace->updated_at)));
    lines.push_back(text("cwd: " + GetFieldOrNA(currentWorkspace->cwd)));

    return window(text("Session Detail"), vbox(lines));
}

static Element RenderDeleteDialog(bool deleteYesSelected)
{
    Element yesButton = text(deleteYesSelected ? "[ YES ]" : "  YES  ");
    Element noButton = text(deleteYesSelected ? "  NO  " : "[ NO ]");

    if (deleteYesSelected)
    {
        yesButton = yesButton | color(Color::RedLight) | bold;
        noButton = noButton | color(Color::White);
    }
    else
    {
        yesButton = yesButton | color(Color::White);
        noButton = noButton | color(Color::Cyan) | bold;
    }

    Element dialogBody = window(
        text("Delete Confirmation") | bold,
        vbox({
            text("Do you really want to delete this conversation?"),
            separator(),
            hbox({
                filler(),
                yesButton,
                text("   "),
                noButton,
                filler(),
            }),
            })
            );

    Element dialog = dialogBody
        | bgcolor(Color::Black)
        | color(Color::White)
        | size(WIDTH, EQUAL, 54);

    return vbox({
        filler(),
        hbox({
            filler(),
            dialog,
            filler(),
        }),
        filler(),
        });
}

TuiAction ShowTuiAndWait(
    const vector<Workspace>& allWorkspace,
    size_t selectedIndex,
    size_t topVisibleIndex,
    const string& statusText,
    PageMode currentPage,
    bool showDeleteDialog,
    bool deleteYesSelected
)
{
    TuiAction action;
    action.type = TuiNone;

    ScreenInteractive screen = ScreenInteractive::FullscreenAlternateScreen();

    Component root = Renderer([&]
        {
            const Workspace* currentWorkspace = nullptr;
            if (!allWorkspace.empty() && selectedIndex < allWorkspace.size())
            {
                currentWorkspace = &allWorkspace[selectedIndex];
            }

            bool muted = showDeleteDialog;
            Element body;

            if (currentPage == PageList)
            {
                body = RenderListPage(allWorkspace, selectedIndex, topVisibleIndex, muted);
            }
            else
            {
                body = RenderDetailPage(currentWorkspace);
                if (muted)
                {
                    body = body | color(Color::GrayDark);
                }
            }

            Element helpBar = RenderHelpBar(currentPage, showDeleteDialog);
            if (muted)
            {
                helpBar = helpBar | color(Color::GrayLight);
            }

            Element page = vbox({
                text("Copilot CLI Conversation History") | bold | center,
                separator(),
                body | flex,
                separator(),
                RenderStatusBar(statusText, muted),
                separator(),
                helpBar,
                }) | border;

            if (showDeleteDialog)
            {
                return dbox({
                    page | color(Color::GrayDark),
                    RenderDeleteDialog(deleteYesSelected)
                    });
            }

            return page;
        });

    Component app = CatchEvent(root, [&](Event event)
        {
            if (event == Event::Character("q") || event == Event::Character("Q"))
            {
                action.type = TuiQuit;
                screen.ExitLoopClosure()();
                return true;
            }

            if (showDeleteDialog)
            {
                if (event == Event::ArrowLeft)
                {
                    action.type = TuiDeleteSelectLeft;
                    screen.ExitLoopClosure()();
                    return true;
                }

                if (event == Event::ArrowRight)
                {
                    action.type = TuiDeleteSelectRight;
                    screen.ExitLoopClosure()();
                    return true;
                }

                if (event == Event::Escape)
                {
                    action.type = TuiCloseDeleteDialog;
                    screen.ExitLoopClosure()();
                    return true;
                }

                if (event == Event::Return)
                {
                    if (deleteYesSelected)
                    {
                        action.type = TuiConfirmDelete;
                    }
                    else
                    {
                        action.type = TuiCloseDeleteDialog;
                    }

                    screen.ExitLoopClosure()();
                    return true;
                }

                return false;
            }

            if (currentPage == PageList)
            {
                if (event == Event::ArrowUp)
                {
                    action.type = TuiMoveUp;
                    screen.ExitLoopClosure()();
                    return true;
                }

                if (event == Event::ArrowDown)
                {
                    action.type = TuiMoveDown;
                    screen.ExitLoopClosure()();
                    return true;
                }

                if (event == Event::Return && !allWorkspace.empty())
                {
                    action.type = TuiOpenDetail;
                    screen.ExitLoopClosure()();
                    return true;
                }

                if (event == Event::Delete && !allWorkspace.empty())
                {
                    action.type = TuiOpenDeleteDialog;
                    screen.ExitLoopClosure()();
                    return true;
                }
            }
            else
            {
                if (event == Event::Escape)
                {
                    action.type = TuiBack;
                    screen.ExitLoopClosure()();
                    return true;
                }

                if (event == Event::Delete && !allWorkspace.empty())
                {
                    action.type = TuiOpenDeleteDialog;
                    screen.ExitLoopClosure()();
                    return true;
                }
            }

            return false;
        });

    screen.Loop(app);
    return action;
}
