#include "MaterialDialog.h"

#if WITH_DEBUG
#include "../../Resource.h"
#include "../../Runtime/Engine/Asset.h"
#include "../../Runtime/Engine/Input.h"
#include "../Classes/EditorUtils.h"
#include "../Classes/ParameterNodeGUI.h"
#include "../Classes/MathNodeGUI.h"
#include "../Classes/TextureNodeGUI.h"
#include "../../Runtime/Classes/Material.h"

enum UHNodeMenuAction
{
    NoAction = 50,
    Deletion,
    Disconnect,

    // add node must be put at the bottom for some reasons
    AddNode,
};

// a few global defines for window proc stuff
struct UHMaterialDialogData
{
    UHMaterialDialogData()
        : Window(nullptr)
        , WorkArea(nullptr)
        , WorkAreaMemDC(nullptr)
        , WorkAreaBmp(nullptr)
        , InitWindowRect(RECT{})
        , InitWorkAreaRect(RECT{})
        , NodeMenuAction(UHNodeMenuAction::NoAction)
        , bNeedRepaint(false)
        , CompileFlag(UpToDate)
        , bNeedResave(false)
    {
    }

    HWND Window;
    HWND WorkArea;
    HDC WorkAreaMemDC;
    HBITMAP WorkAreaBmp;
    RECT InitWindowRect;
    RECT InitWorkAreaRect;
    RECT CurrentWorkAreaRect;
    UHRawInput Input;
    int32_t NodeMenuAction;
    bool bNeedRepaint;
    UHMaterialCompileFlag CompileFlag;
    bool bNeedResave;
};
std::unique_ptr<UHMaterialDialogData> GMaterialDialogData;

UHMaterialDialog::UHMaterialDialog()
	: UHMaterialDialog(nullptr, nullptr, nullptr)
{

}

UHMaterialDialog::UHMaterialDialog(HINSTANCE InInstance, HWND InWindow, UHAssetManager* InAssetManager)
	: UHDialog(InInstance, InWindow)
    , AssetManager(InAssetManager)
    , GUIToMove(nullptr)
    , MousePos(POINT())
    , MousePosWhenRightDown(POINT())
    , NodeToDelete(nullptr)
    , PinToDisconnect(nullptr)
    , CurrentMaterialIndex(-1)
    , CurrentMaterial(nullptr)
    , GdiplusToken(0)
{
    // create popup menu for node functions, only do these in construction time
    ParameterMenu.InsertOption("Float", UHGraphNodeType::Float);
    ParameterMenu.InsertOption("Float2", UHGraphNodeType::Float2);
    ParameterMenu.InsertOption("Float3", UHGraphNodeType::Float3);
    ParameterMenu.InsertOption("Float4", UHGraphNodeType::Float4);

    TextureMenu.InsertOption("Texture2D", UHGraphNodeType::Texture2DNode);

    AddNodeMenu.InsertOption("Parameter", 0, ParameterMenu.GetMenu());
    AddNodeMenu.InsertOption("Math", UHGraphNodeType::MathNode);
    AddNodeMenu.InsertOption("Texture", 0, TextureMenu.GetMenu());

    NodeFunctionMenu.InsertOption("Add node", 0, AddNodeMenu.GetMenu());
    NodeFunctionMenu.InsertOption("Delete node", UHNodeMenuAction::Deletion);
    NodePinMenu.InsertOption("Disconnect", UHNodeMenuAction::Disconnect);

    GdiplusStartup(&GdiplusToken, &GdiplusStartupInput, NULL);
}

UHMaterialDialog::~UHMaterialDialog()
{
    // clear GUI
    for (std::unique_ptr<UHGraphNodeGUI>& GUI : EditNodeGUIs)
    {
        GUI.reset();
    }
    EditNodeGUIs.clear();
    
    GdiplusShutdown(GdiplusToken);
}

void CreateWorkAreaMemDC(int32_t Width, int32_t Height)
{
    // create mem DC / bitmap for workarea
    HDC WorkAreaDC = GetDC(GMaterialDialogData->WorkArea);
    GMaterialDialogData->WorkAreaMemDC = CreateCompatibleDC(WorkAreaDC);
    GMaterialDialogData->WorkAreaBmp = CreateCompatibleBitmap(WorkAreaDC
        , Width
        , Height);
    ReleaseDC(GMaterialDialogData->WorkArea, WorkAreaDC);

    // store latest work area rect
    UHEditorUtil::GetWindowSize(GMaterialDialogData->WorkArea, GMaterialDialogData->CurrentWorkAreaRect, GMaterialDialogData->Window);
}

// Message handler for material window
INT_PTR CALLBACK MaterialProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            DeleteDC(GMaterialDialogData->WorkAreaMemDC);
            DeleteObject(GMaterialDialogData->WorkAreaBmp);
            GMaterialDialogData.reset();
            GPinSelectInfo.reset();
            return (INT_PTR)TRUE;
        }
        else if (HIWORD(wParam) == BN_CLICKED)
        {
            int32_t MatIndex = UHEditorUtil::GetListBoxSelectedIndex(GMaterialDialogData->Window, IDC_MATERIAL_LIST);

            if (LOWORD(wParam) == IDC_MATERIALCOMPILE && MatIndex != -1)
            {
                GMaterialDialogData->CompileFlag = FullCompile;
                return (INT_PTR)TRUE;
            }
            else if (LOWORD(wParam) == IDC_MATERIALSAVE && MatIndex != -1)
            {
                GMaterialDialogData->bNeedResave = true;
                return (INT_PTR)TRUE;
            }

            // choose the add node menu action
            GMaterialDialogData->NodeMenuAction = static_cast<int32_t>(LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        return (INT_PTR)FALSE;

    case WM_SIZE:
        {
            uint32_t NewWidth = LOWORD(lParam);
            uint32_t NewHeight = HIWORD(lParam);
            uint32_t GroupBoxWidth = GMaterialDialogData->InitWorkAreaRect.right - GMaterialDialogData->InitWorkAreaRect.left;
            uint32_t GroupBoxHeight = GMaterialDialogData->InitWorkAreaRect.bottom - GMaterialDialogData->InitWorkAreaRect.top;
            uint32_t MaterialWndWidth = GMaterialDialogData->InitWindowRect.right - GMaterialDialogData->InitWindowRect.left;
            uint32_t MaterialWndHeight = GMaterialDialogData->InitWindowRect.bottom - GMaterialDialogData->InitWindowRect.top;

            UHEditorUtil::SetWindowSize(GMaterialDialogData->WorkArea, GMaterialDialogData->InitWorkAreaRect.left, GMaterialDialogData->InitWorkAreaRect.top
                , NewWidth - (MaterialWndWidth - GroupBoxWidth), NewHeight - (MaterialWndHeight - GroupBoxHeight));

            // resize bitmap
            DeleteDC(GMaterialDialogData->WorkAreaMemDC);
            DeleteObject(GMaterialDialogData->WorkAreaBmp);
            CreateWorkAreaMemDC(NewWidth - (MaterialWndWidth - GroupBoxWidth), NewHeight - (MaterialWndHeight - GroupBoxHeight));

            GMaterialDialogData->bNeedRepaint = true;

            return (INT_PTR)TRUE;
        }

    case WM_LBUTTONDOWN:
        GMaterialDialogData->Input.SetLeftMousePressed(true);
        SetCapture(GMaterialDialogData->Window);
        return (INT_PTR)TRUE;

    case WM_RBUTTONDOWN:
        GMaterialDialogData->Input.SetRightMousePressed(true);
        SetCapture(GMaterialDialogData->Window);
        return (INT_PTR)TRUE;

    case WM_LBUTTONUP:
        GMaterialDialogData->Input.SetLeftMousePressed(false);
        ReleaseCapture();
        return (INT_PTR)TRUE;

    case WM_RBUTTONUP:
        GMaterialDialogData->Input.SetRightMousePressed(false);
        ReleaseCapture();
        return (INT_PTR)TRUE;

    case WM_PAINT:
        PAINTSTRUCT Ps;
        HDC Hdc = BeginPaint(hDlg, &Ps);

        // copy drawing result to work area
        RECT R;
        UHEditorUtil::GetWindowSize(GMaterialDialogData->WorkArea, R, GMaterialDialogData->Window);
        BitBlt(Hdc, R.left, R.top, R.right - R.left, R.bottom - R.top
           , GMaterialDialogData->WorkAreaMemDC, 0, 0, SRCCOPY);

        EndPaint(hDlg, &Ps);
        return (INT_PTR)TRUE;
    }
    return (INT_PTR)FALSE;
}

void UHMaterialDialog::ShowDialog()
{
    if (GMaterialDialogData == nullptr)
    {
        Init();
        ShowWindow(GMaterialDialogData->Window, SW_SHOW);
    }
}

void UHMaterialDialog::Update()
{
    if (GMaterialDialogData == nullptr)
    {
        return;
    }

    // store mouse states
    GetCursorPos(&MousePos);
    if (GMaterialDialogData->Input.IsRightMouseDown())
    {
        MousePosWhenRightDown = MousePos;
    }

    // get current selected material index
    int32_t MatIndex = UHEditorUtil::GetListBoxSelectedIndex(GMaterialDialogData->Window, IDC_MATERIAL_LIST);

    if (MatIndex != CurrentMaterialIndex)
    {
        // select material
        SelectMaterial(MatIndex);
        CurrentMaterialIndex = MatIndex;
        GMaterialDialogData->bNeedRepaint = true;
    }

    // force a invalidate after resizing
    if (GMaterialDialogData->bNeedRepaint)
    {
        InvalidateRect(GMaterialDialogData->Window, &GMaterialDialogData->CurrentWorkAreaRect, false);
        GMaterialDialogData->bNeedRepaint = false;
    }

    // only do node operation when a material is selected
    if (MatIndex > -1)
    {
        TryAddNodes();
        TryDeleteNodes();
        TryDisconnectPin();
        TryMoveNodes();
        TryConnectNodes();
        DrawPinConnectionLine();
        ProcessPopMenu();

        if (GMaterialDialogData->CompileFlag != UpToDate)
        {
            RecompileMaterial();
            GMaterialDialogData->CompileFlag = UpToDate;
        }

        // for now it will resave all materials
        if (GMaterialDialogData->bNeedResave)
        {
            ResaveMaterial();
            GMaterialDialogData->bNeedResave = false;
        }
    }

    GMaterialDialogData->Input.CacheKeyStates();
}

void UHMaterialDialog::Init()
{
    GMaterialDialogData = std::make_unique<UHMaterialDialogData>();
    GMaterialDialogData->Window = CreateDialog(Instance, MAKEINTRESOURCE(IDD_MATERIAL), Window, (DLGPROC)MaterialProc);
    GMaterialDialogData->WorkArea = GetDlgItem(GMaterialDialogData->Window, IDC_MATERIAL_GRAPHAREA);
    GMaterialDialogData->Input.ResetMouseState();
    GPinSelectInfo = std::make_unique<UHPinSelectInfo>();

    // get the material list from assets manager
    const std::vector<UHMaterial*>& Materials = AssetManager->GetMaterials();
    for (const UHMaterial* Mat : Materials)
    {
        UHEditorUtil::AddListBoxString(GMaterialDialogData->Window, IDC_MATERIAL_LIST, UHUtilities::ToStringW(Mat->GetName()));
    }

    // cache init window size for resizing purpose
    UHEditorUtil::GetWindowSize(GMaterialDialogData->Window, GMaterialDialogData->InitWindowRect, nullptr);
    UHEditorUtil::GetWindowSize(GMaterialDialogData->WorkArea, GMaterialDialogData->InitWorkAreaRect, GMaterialDialogData->Window);

    // create mem DC / bitmap for workarea
    CreateWorkAreaMemDC(GMaterialDialogData->InitWorkAreaRect.right - GMaterialDialogData->InitWorkAreaRect.left
        , GMaterialDialogData->InitWorkAreaRect.bottom - GMaterialDialogData->InitWorkAreaRect.top);

    // reset material selection
    CurrentMaterialIndex = -1;
    CurrentMaterial = nullptr;
}

void UHMaterialDialog::SelectMaterial(int32_t MatIndex)
{
    CurrentMaterial = AssetManager->GetMaterials()[MatIndex];

    // initialize GUI
    for (std::unique_ptr<UHGraphNodeGUI>& GUI : EditNodeGUIs)
    {
        GUI.reset();
    }
    EditNodeGUIs.clear();

    // init node GUI
    POINT MaterialNodePos = CurrentMaterial->GetDefaultMaterialNodePos();
    EditNodeGUIs.push_back(std::move(std::make_unique<UHMaterialNodeGUI>()));
    EditNodeGUIs[0]->Init(Instance, GMaterialDialogData->WorkArea, CurrentMaterial->GetMaterialNode().get(), "Material Inputs", MaterialNodePos.x, MaterialNodePos.y);

    const std::vector<std::unique_ptr<UHGraphNode>>& EditNodes = CurrentMaterial->GetEditNodes();
    const std::vector<POINT>& GUIRelativePos = CurrentMaterial->GetGUIRelativePos();

    for (size_t Idx = 0; Idx < EditNodes.size(); Idx++)
    {
        GMaterialDialogData->NodeMenuAction = EditNodes[Idx].get()->GetType();
        TryAddNodes(EditNodes[Idx].get(), GUIRelativePos[Idx]);
    }

    // mark pin button state for both material node and edit nodes
    for (std::unique_ptr<UHGraphPin>& Input : CurrentMaterial->GetMaterialNode()->GetInputs())
    {
        if (UHGraphPin* Pin = Input->GetSrcPin())
        {
            Button_SetCheck(Input->GetPinGUI(), BST_CHECKED);
            Button_SetCheck(Pin->GetPinGUI(), BST_CHECKED);
        }
    }

    for (std::unique_ptr<UHGraphNode>& Node : CurrentMaterial->GetEditNodes())
    {
        for (std::unique_ptr<UHGraphPin>& Input : Node->GetInputs())
        {
            if (UHGraphPin* Pin = Input->GetSrcPin())
            {
                Button_SetCheck(Input->GetPinGUI(), BST_CHECKED);
                Button_SetCheck(Pin->GetPinGUI(), BST_CHECKED);
            }
        }
    }
}

void UHMaterialDialog::TryAddNodes(UHGraphNode* InputNode, POINT GUIRelativePos)
{
    // Node menu action for adding could be from individual node type
    // return if it's not adding nodes, this also means AddNode needs to be put the bottom of UHNodeMenuAction
    if (GMaterialDialogData->NodeMenuAction < UHNodeMenuAction::AddNode)
    {
        return;
    }

    std::unique_ptr<UHGraphNode> NewNode = AllocateNewGraphNode(static_cast<UHGraphNodeType>(GMaterialDialogData->NodeMenuAction));
    std::unique_ptr<UHGraphNodeGUI> NewGUI;
    std::string GUIName = "";

    switch (GMaterialDialogData->NodeMenuAction)
    {
    case UHGraphNodeType::Float:
        NewGUI = std::make_unique<UHFloatNodeGUI>();
        GUIName = "Float";
        break;
    case UHGraphNodeType::Float2:
        NewGUI = std::make_unique<UHFloat2NodeGUI>();
        GUIName = "Float2";
        break;
    case UHGraphNodeType::Float3:
        NewGUI = std::make_unique<UHFloat3NodeGUI>();
        GUIName = "Float3";
        break;
    case UHGraphNodeType::Float4:
        NewGUI = std::make_unique<UHFloat4NodeGUI>();
        GUIName = "Float4";
        break;
    case UHGraphNodeType::MathNode:
        NewGUI = std::make_unique<UHMathNodeGUI>();
        GUIName = "Math";
        break;
    case UHGraphNodeType::Texture2DNode:
        NewGUI = std::make_unique<UHTexture2DNodeGUI>(AssetManager, CurrentMaterial);
        GUIName = "Texture2D";
        break;
    }

    // add GUI only if there is an input node, otherwise adding a new node
    if (InputNode)
    {
        POINT P = CurrentMaterial->GetDefaultMaterialNodePos();
        NewNode.reset();
        NewGUI->Init(Instance, GMaterialDialogData->WorkArea, InputNode, GUIName, P.x + GUIRelativePos.x, P.y + GUIRelativePos.y);

        EditNodeGUIs.push_back(std::move(NewGUI));
    }
    else if (NewNode)
    {
        CurrentMaterial->GetEditNodes().push_back(std::move(NewNode));

        POINT P = MousePosWhenRightDown;
        ScreenToClient(GMaterialDialogData->WorkArea, &P);
        NewGUI->Init(Instance, GMaterialDialogData->WorkArea, CurrentMaterial->GetEditNodes().back().get(), GUIName, P.x, P.y);

        EditNodeGUIs.push_back(std::move(NewGUI));
    }

    GMaterialDialogData->NodeMenuAction = UHNodeMenuAction::NoAction;
}

void UHMaterialDialog::TryDeleteNodes()
{
    if (GMaterialDialogData->NodeMenuAction != UHNodeMenuAction::Deletion || NodeToDelete == nullptr)
    {
        return;
    }

    // disconnect all input pin GUI
    for (std::unique_ptr<UHGraphPin>& InputPin : NodeToDelete->GetInputs())
    {
        if (UHGraphPin* SrcPin = InputPin->GetSrcPin())
        {
            Button_SetCheck(SrcPin->GetPinGUI(), BST_UNCHECKED);
        }
    }

    // disconnect all output pin GUI
    for (std::unique_ptr<UHGraphPin>& OutputPin : NodeToDelete->GetOutputs())
    {
        for (UHGraphPin* DestPin : OutputPin->GetDestPins())
        {
            DestPin->Disconnect();
            Button_SetCheck(DestPin->GetPinGUI(), BST_UNCHECKED);
        }
    }

    // delete node
    for (int32_t Idx = static_cast<int32_t>(CurrentMaterial->GetEditNodes().size() - 1); Idx >= 0; Idx--)
    {
        if (CurrentMaterial->GetEditNodes()[Idx]->GetId() == NodeToDelete->GetId())
        {
            CurrentMaterial->GetEditNodes().erase(CurrentMaterial->GetEditNodes().begin() + Idx);
            break;
        }
    }

    // also delete GUI
    for (int32_t Idx = static_cast<int32_t>(EditNodeGUIs.size() - 1); Idx >= 0; Idx--)
    {
        if (EditNodeGUIs[Idx]->GetNode()->GetId() == NodeToDelete->GetId())
        {
            EditNodeGUIs.erase(EditNodeGUIs.begin() + Idx);
            break;
        }
    }

    GMaterialDialogData->NodeMenuAction = UHNodeMenuAction::NoAction;
    NodeToDelete = nullptr;
    GMaterialDialogData->bNeedRepaint = true;
}

void UHMaterialDialog::TryDisconnectPin()
{
    if (GMaterialDialogData->NodeMenuAction != UHNodeMenuAction::Disconnect || PinToDisconnect == nullptr)
    {
        return;
    }

    // disconnect an input pin
    Button_SetCheck(PinToDisconnect->GetPinGUI(), BST_UNCHECKED);
    if (PinToDisconnect->GetSrcPin())
    {
        Button_SetCheck(PinToDisconnect->GetSrcPin()->GetPinGUI(), BST_UNCHECKED);
        PinToDisconnect->Disconnect();
    }
    else
    {
        // disconnect an output pin
        for (UHGraphPin* DestPin : PinToDisconnect->GetDestPins())
        {
            Button_SetCheck(DestPin->GetPinGUI(), BST_UNCHECKED);
            PinToDisconnect->Disconnect(DestPin->GetId());
            DestPin->Disconnect();
        }
    }

    GMaterialDialogData->NodeMenuAction = UHNodeMenuAction::NoAction;
    PinToDisconnect = nullptr;
    GMaterialDialogData->bNeedRepaint = true;
}

void UHMaterialDialog::TryMoveNodes()
{
    // select node to move first
    if (GMaterialDialogData->Input.IsLeftMouseDown())
    {
        // move one GUI at the same time
        for (std::unique_ptr<UHGraphNodeGUI>& GUI : EditNodeGUIs)
        {
            if (GUI->IsPointInside(MousePos))
            {
                GUIToMove = GUI->GetHWND();
                break;
            }
        }
    }
    else if (GMaterialDialogData->Input.IsLeftMouseUp())
    {
        GUIToMove = nullptr;
    }

    // function for move GUI
    auto MoveGUI = [](HWND GUIToMove, uint32_t Dx, uint32_t Dy)
    {
        RECT R;
        UHEditorUtil::GetWindowSize(GUIToMove, R, GMaterialDialogData->WorkArea);
        MoveWindow(GUIToMove, R.left + Dx, R.top + Dy, R.right - R.left, R.bottom - R.top, false);
    };

    // cache mouse movement before doing any operations
    uint32_t MouseDeltaX;
    uint32_t MouseDeltaY;
    GMaterialDialogData->Input.GetMouseDelta(MouseDeltaX, MouseDeltaY);

    if (UHEditorUtil::IsPointInsideClient(GMaterialDialogData->WorkArea, MousePos))
    {
        if (GMaterialDialogData->Input.IsLeftMouseHold() && GUIToMove != nullptr)
        {
            // move node
            MoveGUI(GUIToMove, MouseDeltaX, MouseDeltaY);

            GMaterialDialogData->bNeedRepaint = true;
        }
        else if (GMaterialDialogData->Input.IsRightMouseHold())
        {
            // move all nodes as like we're moving the view client
            for (std::unique_ptr<UHGraphNodeGUI>& GUI : EditNodeGUIs)
            {
                MoveGUI(GUI->GetHWND(), MouseDeltaX, MouseDeltaY);
            }

            GMaterialDialogData->bNeedRepaint = true;
        }
    }
}

void UHMaterialDialog::TryConnectNodes()
{
    if (!GPinSelectInfo->bReadyForConnect)
    {
        return;
    }

    // ready for connect, find the GUI we're connecting to
    for (std::unique_ptr<UHGraphNodeGUI>& NodeGUI : EditNodeGUIs)
    {
        // can not connect to self of course..
        if (NodeGUI->GetNode() == GPinSelectInfo->CurrOutputPin->GetOriginNode())
        {
            continue;
        }

        int32_t DestPinIndex;
        if (UHGraphPin* DestPin = NodeGUI->GetInputPinByMousePos(GPinSelectInfo->MouseUpPos, DestPinIndex))
        {
            UHGraphPin* OldSrcPin = DestPin->GetSrcPin();
            bool bConnectSucceed = DestPin->ConnectFrom(GPinSelectInfo->CurrOutputPin);

            if (bConnectSucceed)
            {
                // mark radio button
                Button_SetCheck(GPinSelectInfo->CurrOutputPin->GetPinGUI(), BST_CHECKED);
                Button_SetCheck(DestPin->GetPinGUI(), BST_CHECKED);

                if (OldSrcPin && OldSrcPin->GetDestPins().size() == 0)
                {
                    Button_SetCheck(OldSrcPin->GetPinGUI(), BST_UNCHECKED);
                }
            }
        }
    }

    // clear the selection in the end
    GPinSelectInfo->bReadyForConnect = false;
    GPinSelectInfo->CurrOutputPin = nullptr;
    GMaterialDialogData->bNeedRepaint = true;
}

void UHMaterialDialog::ProcessPopMenu()
{
    if (GMaterialDialogData->Input.IsRightMouseUp() && UHEditorUtil::IsPointInsideClient(GMaterialDialogData->WorkArea, MousePos))
    {
        if (MousePosWhenRightDown.x == MousePos.x && MousePosWhenRightDown.y == MousePos.y)
        {
            // see if there is a node to delete
            for (std::unique_ptr<UHGraphNodeGUI>& GUI : EditNodeGUIs)
            {
                if (GUI->IsPointInside(MousePos) && GUI->GetNode()->CanBeDeleted())
                {
                    NodeToDelete = GUI->GetNode();
                    break;
                }
            }

            NodeFunctionMenu.SetOptionActive(1, NodeToDelete != nullptr);
            NodeFunctionMenu.ShowMenu(GMaterialDialogData->Window, MousePos.x, MousePos.y);
        }
    }
    else if (GPinSelectInfo->RightClickedPin != nullptr)
    {
        // process pin disconnect menu
        NodePinMenu.ShowMenu(GMaterialDialogData->Window, MousePos.x, MousePos.y);
        PinToDisconnect = GPinSelectInfo->RightClickedPin;
        GPinSelectInfo->RightClickedPin = nullptr;
    }
}

void UHMaterialDialog::DrawPinConnectionLine()
{
    SelectObject(GMaterialDialogData->WorkAreaMemDC, GMaterialDialogData->WorkAreaBmp);
    Graphics Graphics(GMaterialDialogData->WorkAreaMemDC);
    Pen Pen(Color(255, 0, 0, 255));
    Graphics.Clear(Color(255, 240, 240, 240));

    // draw line when user is dragging
    if (GPinSelectInfo->CurrOutputPin)
    {
        // draw line, point is relative to window 
        POINT P1 = GPinSelectInfo->MouseDownPos;
        POINT P2 = MousePos;
        ScreenToClient(GMaterialDialogData->WorkArea, &P1);
        ScreenToClient(GMaterialDialogData->WorkArea, &P2);

        Graphics.DrawLine(&Pen, (int32_t)P1.x, (int32_t)P1.y, (int32_t)P2.x, (int32_t)P2.y);
        GMaterialDialogData->bNeedRepaint = true;
    }

    // draw line for connected pins
    for (std::unique_ptr<UHGraphNodeGUI>& NodeGUI : EditNodeGUIs)
    {
        UHGraphNode* SrcNode = NodeGUI->GetNode();

        const std::vector<std::unique_ptr<UHGraphPin>>& Inputs = SrcNode->GetInputs();
        for (int32_t Idx = 0; Idx < static_cast<int32_t>(Inputs.size()); Idx++)
        {
            // only draw when the pin is connected
            if (UHGraphPin* Pin = Inputs[Idx]->GetSrcPin())
            {
                HWND Src = Pin->GetPinGUI();
                HWND Dst = NodeGUI->GetInputPin(Idx);

                RECT R1{};
                RECT R2{};
                GetWindowRect(Src, &R1);
                GetWindowRect(Dst, &R2);

                POINT P1{};
                POINT P2{};
                P1.x = R1.right;
                P1.y = (R1.top + R1.bottom) / 2;
                P2.x = R2.left;
                P2.y = (R2.top + R2.bottom) / 2;

                ScreenToClient(GMaterialDialogData->WorkArea, &P1);
                ScreenToClient(GMaterialDialogData->WorkArea, &P2);

                Graphics.DrawLine(&Pen, (int32_t)P1.x, (int32_t)P1.y, (int32_t)P2.x, (int32_t)P2.y);
            }
        }
    }
}

void UHMaterialDialog::RecompileMaterial()
{
    if (CurrentMaterial)
    {
        for (std::unique_ptr<UHGraphNodeGUI>& GUI : EditNodeGUIs)
        {
            GUI->SetDefaultValueFromGUI();
        }
        CurrentMaterial->SetCompileFlag(GMaterialDialogData->CompileFlag);
    }
}

void UHMaterialDialog::ResaveMaterial()
{
    // save current selected material
    // also sync the GUI position
    RECT Rect;
    UHEditorUtil::GetWindowSize(EditNodeGUIs[0]->GetHWND(), Rect, GMaterialDialogData->WorkArea);
    POINT Pos{};
    Pos.x = Rect.left;
    Pos.y = Rect.top;
    CurrentMaterial->SetDefaultMaterialNodePos(Pos);

    std::vector<POINT> EditGUIPos;
    for (size_t Idx = 1; Idx < EditNodeGUIs.size(); Idx++)
    {
        UHEditorUtil::GetWindowSize(EditNodeGUIs[Idx]->GetHWND(), Rect, EditNodeGUIs[0]->GetHWND());
        Pos.x = Rect.left;
        Pos.y = Rect.top;
        EditGUIPos.push_back(Pos);
    }
    CurrentMaterial->SetGUIRelativePos(EditGUIPos);

    CurrentMaterial->Export();

    std::string Msg = CurrentMaterial->GetName();
    Msg += " is saved.";
    MessageBoxA(GMaterialDialogData->Window, Msg.c_str(), "Material Editor", MB_OK);
}

#endif