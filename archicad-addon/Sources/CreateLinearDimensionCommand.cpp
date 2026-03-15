#include "CreateLinearDimensionCommand.hpp"
#include "ObjectState.hpp"
#include "ACAPinc.h"
#include "MigrationHelper.hpp"

#include <cmath>

static const char*   DEFAULT_DIMENSION_FONT      = "Arial";
static const double  DEFAULT_TEXT_SIZE_METERS     = 0.003; // 3 mm nominal text height

// ── Helpers ───────────────────────────────────────────────────────────────────

static API_ElemTypeID GetElemTypeID (const API_Guid& guid)
{
    API_Element elem = {};
    elem.header.guid = guid;
    if (ACAPI_Element_Get (&elem) != NoError)
        return API_ZombieElemID;
    return GetElemTypeId (elem.header);
}

static API_Neig MakeNeig (const API_Guid& elemGuid, Int32 neigIndex, API_ElemTypeID typeID)
{
    API_Neig neig = {};

    switch (typeID) {
        case API_WallID:   neig.neigID = APINeig_WallOn;  break;
        case API_SlabID:   neig.neigID = APINeig_SlabOn;  break;
        case API_ColumnID: neig.neigID = APINeig_ColumOn; break;
        case API_BeamID:   neig.neigID = APINeig_BeamOn;  break;
        default:           neig.neigID = APINeig_None;    break;
    }

    neig.guid    = elemGuid;
    neig.inIndex = neigIndex;
    return neig;
}

// ── Command ───────────────────────────────────────────────────────────────────

CreateLinearDimensionCommand::CreateLinearDimensionCommand ()
    : CommandBase (CommonSchema::NotUsed)
{}

GS::String CreateLinearDimensionCommand::GetName () const
{
    return "CreateLinearDimension";
}

GS::Optional<GS::UniString> CreateLinearDimensionCommand::GetInputParametersSchema () const
{
    return GS::UniString (R"JSON({
        "type": "object",
        "properties": {
            "dimensionPoints": {
                "type": "array",
                "description": "Ordered list of points to dimension. Each point requires x/y coordinates. Optionally include elementGuid + neigIndex for associative dimensioning.",
                "items": {
                    "type": "object",
                    "properties": {
                        "x":           { "type": "number",  "description": "X coordinate in meters" },
                        "y":           { "type": "number",  "description": "Y coordinate in meters" },
                        "elementGuid": { "type": "string",  "description": "GUID of the element to associate (optional)" },
                        "neigIndex":   { "type": "integer", "description": "Geometry point index on element. Wall: 1=refStart,2=refEnd,11=oppStart,21=oppEnd. Slab: vertex 1-N. Column: 1. Beam: 1=start,2=end." }
                    },
                    "required": ["x", "y"]
                },
                "minItems": 2
            },
            "refLineOffset": {
                "type": "number",
                "description": "Perpendicular offset of the dimension line in meters. Positive or negative controls which side. Typical value: 0.5 or -0.5."
            },
            "isHorizontal": {
                "type": "boolean",
                "description": "true = horizontal chain (measures X distances). false = vertical chain (measures Y distances). Auto-detected from points if omitted."
            },
            "linPen":     { "type": "integer", "description": "Pen index for dimension lines (1-255). Default: 1." },
            "textPen":    { "type": "integer", "description": "Pen index for dimension text (1-255). Default: 1." },
            "storyIndex": { "type": "integer", "description": "Story index for placement. Default: 0 = active story." }
        },
        "required": ["dimensionPoints", "refLineOffset"]
    })JSON");
}

GS::Optional<GS::UniString> CreateLinearDimensionCommand::GetResponseSchema () const
{
    return GS::UniString (R"JSON({
        "type": "object",
        "properties": {
            "dimensionGuid": { "type": "string",  "description": "GUID of the created dimension element." },
            "success":       { "type": "boolean" },
            "error":         { "type": "string",  "description": "Error message if success is false." }
        }
    })JSON");
}

GS::ObjectState CreateLinearDimensionCommand::Execute (
    const GS::ObjectState& parameters,
    GS::ProcessControl& /*processControl*/
) const
{
    // ── 1. Parse parameters ──────────────────────────────────────────────────

    GS::Array<GS::ObjectState> pointsArray;
    if (!parameters.Get ("dimensionPoints", pointsArray) || pointsArray.GetSize () < 2)
        return GS::ObjectState ("success", false, "error", GS::String ("dimensionPoints must have at least 2 items"));

    double refLineOffset = 0.5;
    parameters.Get ("refLineOffset", refLineOffset);

    Int32 linPen = 1, textPen = 1;
    parameters.Get ("linPen",  linPen);
    parameters.Get ("textPen", textPen);

    Int32 storyIndex = 0;
    parameters.Get ("storyIndex", storyIndex);

    bool isHorizontal = true;
    bool hasOrientParam = parameters.Get ("isHorizontal", isHorizontal);
    if (!hasOrientParam && pointsArray.GetSize () >= 2) {
        double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
        pointsArray[0].Get ("x", x0); pointsArray[0].Get ("y", y0);
        pointsArray[1].Get ("x", x1); pointsArray[1].Get ("y", y1);
        isHorizontal = (std::abs (x1 - x0) >= std::abs (y1 - y0));
    }

    // ── 2. Build element header ──────────────────────────────────────────────

    API_Element elem = {};
#ifdef ServerMainVers_2600
    elem.header.type = API_ElemType (API_DimensionID);
#else
    elem.header.typeID = API_DimensionID;
#endif

    if (storyIndex > 0) {
        elem.header.floorInd = (short)storyIndex;
    } else {
        API_StoryInfo storyInfo = {};
        ACAPI_ProjectSetting_GetStorySettings (&storyInfo);
        elem.header.floorInd = storyInfo.actStory;
        BMKillHandle ((GSHandle*)&storyInfo.data);
    }

    API_DimensionType& dim = elem.dimension;
    dim.linPen  = (short)linPen;
    dim.textPos = APIPos_Above;
    dim.textWay = APIDir_Parallel;

    dim.defNote.contentType        = API_NoteContent_Measured;
    dim.defNote.textStyle.pen      = (short)textPen;
    dim.defNote.textStyle.size     = DEFAULT_TEXT_SIZE_METERS;
    CHTruncate (DEFAULT_DIMENSION_FONT, dim.defNote.textStyle.faceName, sizeof (dim.defNote.textStyle.faceName));

    double firstX = 0.0, firstY = 0.0;
    pointsArray[0].Get ("x", firstX);
    pointsArray[0].Get ("y", firstY);

    if (isHorizontal) {
        dim.direction.x = 1.0;
        dim.direction.y = 0.0;
        dim.refPoint.x  = firstX;
        dim.refPoint.y  = firstY + refLineOffset;
    } else {
        dim.direction.x = 0.0;
        dim.direction.y = 1.0;
        dim.refPoint.x  = firstX + refLineOffset;
        dim.refPoint.y  = firstY;
    }

    // ── 3. Build memo (individual dimension points) ──────────────────────────

    API_ElementMemo memo = {};

    USize nPts = pointsArray.GetSize ();
    memo.dimElems = (API_DimElem**)BMAllocateHandle (
        (GSSize)(nPts * sizeof (API_DimElem)), ALLOCATE_CLEAR, 0
    );
    if (memo.dimElems == nullptr) {
        return GS::ObjectState ("success", false, "error", GS::String ("Memory allocation failed"));
    }

    for (UIndex i = 0; i < nPts; ++i) {
        API_DimElem& de = (*memo.dimElems)[i];

        double px = 0.0, py = 0.0;
        pointsArray[i].Get ("x", px);
        pointsArray[i].Get ("y", py);
        de.pos.x = px;
        de.pos.y = py;

        GS::String guidStr;
        if (pointsArray[i].Get ("elementGuid", guidStr) && !guidStr.IsEmpty ()) {
            API_Guid elemGuid = APIGuidFromString (guidStr.ToCStr ());
            API_ElemTypeID typeID = GetElemTypeID (elemGuid);

            Int32 neigIndex = 1;
            pointsArray[i].Get ("neigIndex", neigIndex);

            de.base.base = MakeNeig (elemGuid, neigIndex, typeID);
        }

        de.witnessForm = APIWtn_Large;
        de.witnessVal  = 0.0;
    }

    // ── 4. Create element ────────────────────────────────────────────────────

    GSErrCode err = NoError;
    ACAPI_CallUndoableCommand ("Create Linear Dimension", [&] () -> GSErrCode {
        err = ACAPI_Element_Create (&elem, &memo);
        return err;
    });

    ACAPI_DisposeElemMemoHdls (&memo);

    if (err != NoError) {
        GS::UniString errMsg = GS::UniString::Printf (
            "ACAPI_Element_Create failed: error code %d", (int)err
        );
        return GS::ObjectState ("success", false, "error", errMsg.ToCStr ().Get ());
    }

    return GS::ObjectState (
        "success",       true,
        "dimensionGuid", APIGuidToString (elem.header.guid).ToCStr ().Get ()
    );
}
