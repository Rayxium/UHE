#pragma once
#include "../../../UnheardEngine.h"
#include <vector>
#include <string>
#include <fstream>
#include "GraphPin.h"
#include "../../../Runtime/Classes/Object.h"

class UHGraphNodeGUI;

enum UHGraphNodeType
{
    UnknownNode,
    Float = 100,
    Float2,
    Float3,
    Float4,
    MathNode,
    Texture2DNode
};

// Graph Node Class in UHE, a node can contain multiple inputs and outputs, the name, this is a base class
class UHGraphNode : public UHObject
{
public:
    UHGraphNode(bool bInCanBeDeleted);
    ~UHGraphNode();

    // node functions
    virtual bool CanEvalHLSL() { return true; }
    virtual std::string EvalDefinition() { return ""; }
    virtual std::string EvalHLSL() = 0;
    virtual bool IsEqual(const UHGraphNode* InNode) { return GetId() == InNode->GetId(); }

    // data I/O override
    virtual void InputData(std::ifstream& FileIn) = 0;
    virtual void OutputData(std::ofstream& FileOut) = 0;

    // GUI function for lookup
#if WITH_DEBUG
    void SetGUI(UHGraphNodeGUI* InGUI);
    UHGraphNodeGUI* GetGUI() const;
#endif

    std::string GetName() const;
    UHGraphNodeType GetType() const;
    std::vector<std::unique_ptr<UHGraphPin>>& GetInputs();
    std::vector<std::unique_ptr<UHGraphPin>>& GetOutputs();
    bool CanBeDeleted() const;

protected:
    std::string Name;
    UHGraphNodeType NodeType;
    std::vector<std::unique_ptr<UHGraphPin>> Inputs;
    std::vector<std::unique_ptr<UHGraphPin>> Outputs;
    bool bCanBeDeleted;

#if WITH_DEBUG
    UHGraphNodeGUI* GUICache;
#endif
};