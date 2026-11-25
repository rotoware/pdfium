// Thin C++ primitives exposing PDFium internal APIs to Rust via FFI.
// Declared in pdfium-ffi (src/lib.rs); built into libpdfium.a by //:pdfium.

#include "public/fpdfview.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_ppo.h"
#include "public/fpdf_transformpage.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_stream_acc.h"
#include "core/fpdfapi/parser/fpdf_parser_utility.h"
#include "core/fxcodec/flate/flatemodule.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/data_vector.h"
#include "third_party/zlib/zlib.h"
#include "core/fpdfapi/edit/cpdf_pageexporter.h"
#include "core/fpdfapi/edit/cpdf_pageorganizer.h"
#include "core/fpdfapi/edit/cpdf_npagetooneexporter.h"
#include "core/fpdfapi/page/cpdf_colorspace.h"
#include "core/fpdfapi/page/cpdf_colorstate.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_occontext.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_clippath.h"
#include "core/fpdfapi/page/cpdf_path.h"
#include "core/fxcrt/fx_stream.h"
#include "core/fxge/dib/cfx_cmyk_to_srgb.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#ifdef __cplusplus
extern "C"
{
#endif

    // --- Color primitives ---
    void PDFKING_PageObj_SetFillRGBA(FPDF_PAGEOBJECT obj, float r, float g, float b, float a);
    void PDFKING_PageObj_SetStrokeRGBA(FPDF_PAGEOBJECT obj, float r, float g, float b, float a);
    void PDFKING_PageObj_SetFillCMYKA(FPDF_PAGEOBJECT obj, float c, float m, float y, float k, float a);
    void PDFKING_PageObj_SetStrokeCMYKA(FPDF_PAGEOBJECT obj, float c, float m, float y, float k, float a);
    void PDFKING_CmykToRgb(const uint8_t* cmyk, uint8_t* rgb, uint32_t pixels);
    uint32_t PDFKING_Doc_NewSeparation(void* doc, const char* name, float c, float m, float y, float k);
    void PDFKING_PageObj_SetFillSeparation(FPDF_PAGE page, FPDF_PAGEOBJECT obj, uint32_t objnum, float a);
    void PDFKING_PageObj_SetStrokeSeparation(FPDF_PAGE page, FPDF_PAGEOBJECT obj, uint32_t objnum, float a);

    // --- Clip / transparency primitives ---
    int PDFKING_PageObj_SetClipRect(FPDF_PAGEOBJECT obj, float left, float bottom, float right, float top);
    int PDFKING_XObject_CopyGroup(FPDF_PAGE src_page, FPDF_XOBJECT dest_xobject);

    // --- Document primitives (for page export) ---
    void* PDFKING_Doc_FromFPDF(FPDF_DOCUMENT doc);
    void PDFKING_Doc_SetLastObjNum(void* doc, uint32_t num);
    uint32_t PDFKING_Doc_GetLastObjNum(void* doc);
    int PDFKING_Doc_ExportPage(void* dest, void* src, uint32_t page_index, int insert_index);
    void* PDFKING_Doc_GetObject(void* doc, uint32_t objnum);
    int PDFKING_Object_PageTag(void* obj);
    void PDFKING_Page_SetParentRef(void* obj, void* doc, uint32_t parent_objnum);

    typedef int (*PDFKING_WriteFn)(const uint8_t* data, uint32_t size, void* ctx);
    int PDFKING_Object_Serialize(void* obj, PDFKING_WriteFn cb, void* ctx);

    // --- Catalog primitives: layers and the shared output intent / XMP ---
    void PDFKING_Doc_TagLayers(void* doc);
    int PDFKING_Object_LayerFlags(void* obj);
    int PDFKING_Object_LayerName(void* obj, PDFKING_WriteFn cb, void* ctx);
    void PDFKING_Doc_RemapRefs(void* doc, uint32_t first, uint32_t last, const uint32_t* from, const uint32_t* to, uint32_t count);
    void PDFKING_Object_SetLayerPrint(void* obj, int flags);
    void PDFKING_Doc_SetLayers(void* doc, const uint32_t* objnums, const int32_t* flags, uint32_t count);
    uint32_t PDFKING_Doc_NewLayer(void* doc, const char* name, int flags);
    void PDFKING_PageObj_SetLayer(FPDF_PAGE page, FPDF_PAGEOBJECT obj, uint32_t objnum);
    int PDFKING_Doc_ExportShared(void* dest, void* src, uint32_t* intents, uint32_t* metadata);
    FPDF_PAGE PDFKING_Page_LoadUnparsed(FPDF_DOCUMENT doc, int page_index);

    // --- Object primitives ---
    typedef int (*PDFKING_EntryFn)(const uint8_t* key, uint32_t size, void* value, void* ctx);
    typedef void (*PDFKING_RefFn)(uint32_t objnum, void* ctx);
    int PDFKING_Object_Kind(void* obj);
    void* PDFKING_Object_Direct(void* obj);
    uint32_t PDFKING_Object_Num(void* obj);
    double PDFKING_Object_Number(void* obj);
    int PDFKING_Object_Bytes(void* obj, PDFKING_WriteFn cb, void* ctx);
    int PDFKING_Object_Text(void* obj, PDFKING_WriteFn cb, void* ctx);
    void* PDFKING_Dict_Get(void* obj, const uint8_t* key, uint32_t size);
    void PDFKING_Dict_Each(void* obj, PDFKING_EntryFn cb, void* ctx);
    uint32_t PDFKING_Array_Size(void* obj);
    void* PDFKING_Array_Get(void* obj, uint32_t index);
    int PDFKING_Stream_Data(void* obj, int filtered, PDFKING_WriteFn cb, void* ctx);
    void PDFKING_Object_Refs(void* obj, PDFKING_RefFn cb, void* ctx);
    void* PDFKING_Doc_PageDict(void* doc, uint32_t page_index);
    void* PDFKING_Doc_Root(void* doc);
    int PDFKING_Doc_LayerPrints(void* doc, void* oc);
    void* PDFKING_Dict_New();
    void PDFKING_Object_Release(void* obj);
    void PDFKING_Dict_Set(void* obj, const uint8_t* key, uint32_t size, void* value);
    void PDFKING_Dict_SetClone(void* obj, const uint8_t* key, uint32_t size, void* value);
    void PDFKING_Stream_SetData(void* obj, const uint8_t* data, uint32_t size);
    void* PDFKING_Deflater_New(int level);
    uint32_t PDFKING_Deflater_Segment(void* deflater, const uint8_t* data, uint32_t size, PDFKING_WriteFn cb, void* ctx);
    void PDFKING_Deflater_Free(void* deflater);

#ifdef __cplusplus
}
#endif

// =============================================================================
// CMYK color primitives
// =============================================================================

// `pixels` CMYK pixels (255 the full ink) to RGB, as pdfium rasters DeviceCMYK
void PDFKING_CmykToRgb(const uint8_t* cmyk, uint8_t* rgb, uint32_t pixels) {
    for (uint32_t i = 0; i < pixels; ++i, cmyk += 4, rgb += 3) {
        FX_RGB_STRUCT<uint8_t> c = AdobeCmykToStandardRgb(cmyk[0], cmyk[1], cmyk[2], cmyk[3]);
        rgb[0] = c.red;
        rgb[1] = c.green;
        rgb[2] = c.blue;
    }
}

void PDFKING_PageObj_SetFillRGBA(FPDF_PAGEOBJECT obj, float r, float g, float b, float a) {
    CPDF_PageObject* p = CPDFPageObjectFromFPDFPageObject(obj);
    if (!p) return;
    p->mutable_general_state().SetFillAlpha(a);
    std::vector<float> rgb = {r, g, b};
    p->mutable_color_state().SetFillColor(
        CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceRGB), std::move(rgb));
    p->SetDirty(true);
}

void PDFKING_PageObj_SetStrokeRGBA(FPDF_PAGEOBJECT obj, float r, float g, float b, float a) {
    CPDF_PageObject* p = CPDFPageObjectFromFPDFPageObject(obj);
    if (!p) return;
    p->mutable_general_state().SetStrokeAlpha(a);
    std::vector<float> rgb = {r, g, b};
    p->mutable_color_state().SetStrokeColor(
        CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceRGB), std::move(rgb));
    p->SetDirty(true);
}

void PDFKING_PageObj_SetFillCMYKA(FPDF_PAGEOBJECT obj, float c, float m, float y, float k, float a) {
    CPDF_PageObject* p = CPDFPageObjectFromFPDFPageObject(obj);
    if (!p) return;
    p->mutable_general_state().SetFillAlpha(a);
    std::vector<float> cmyk = {c, m, y, k};
    p->mutable_color_state().SetFillColor(
        CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceCMYK), std::move(cmyk));
    p->SetDirty(true);
}

void PDFKING_PageObj_SetStrokeCMYKA(FPDF_PAGEOBJECT obj, float c, float m, float y, float k, float a) {
    CPDF_PageObject* p = CPDFPageObjectFromFPDFPageObject(obj);
    if (!p) return;
    p->mutable_general_state().SetStrokeAlpha(a);
    std::vector<float> cmyk = {c, m, y, k};
    p->mutable_color_state().SetStrokeColor(
        CPDF_ColorSpace::GetStockCS(CPDF_ColorSpace::Family::kDeviceCMYK), std::move(cmyk));
    p->SetDirty(true);
}

// =============================================================================
// Spot color primitives
// =============================================================================

// A spot ink: [/Separation /<name> /DeviceCMYK fn], fn the tint from no ink
// to `c m y k` — what shows wherever the ink is not
uint32_t PDFKING_Doc_NewSeparation(void* doc, const char* name, float c, float m, float y, float k) {
    CPDF_Document* d = static_cast<CPDF_Document*>(doc);
    RetainPtr<CPDF_Array> space = d->NewIndirect<CPDF_Array>();
    space->AppendNew<CPDF_Name>("Separation");
    space->AppendNew<CPDF_Name>(name);
    space->AppendNew<CPDF_Name>("DeviceCMYK");
    RetainPtr<CPDF_Dictionary> tint = space->AppendNew<CPDF_Dictionary>();
    tint->SetNewFor<CPDF_Number>("FunctionType", 2);
    RetainPtr<CPDF_Array> domain = tint->SetNewFor<CPDF_Array>("Domain");
    domain->AppendNew<CPDF_Number>(0);
    domain->AppendNew<CPDF_Number>(1);
    RetainPtr<CPDF_Array> c0 = tint->SetNewFor<CPDF_Array>("C0");
    RetainPtr<CPDF_Array> c1 = tint->SetNewFor<CPDF_Array>("C1");
    for (float v : {c, m, y, k}) {
        c0->AppendNew<CPDF_Number>(0);
        c1->AppendNew<CPDF_Number>(v);
    }
    tint->SetNewFor<CPDF_Number>("N", 1);
    return space->GetObjNum();
}

// `obj` painted with the Separation `objnum` at full tint: /CS<n> in the
// page's /ColorSpace, the name pdfium's content generator writes it under
static void SetSeparation(FPDF_PAGE page, FPDF_PAGEOBJECT obj, uint32_t objnum, bool stroke, float a) {
    CPDF_Page* p = CPDFPageFromFPDFPage(page);
    CPDF_PageObject* o = CPDFPageObjectFromFPDFPageObject(obj);
    if (!p || !o) return;
    CPDF_Document* d = p->GetDocument();
    RetainPtr<const CPDF_Object> array = d->GetIndirectObject(objnum);
    if (!array) return;
    RetainPtr<CPDF_ColorSpace> space = CPDF_DocPageData::FromDocument(d)->GetColorSpace(array.Get(), nullptr);
    if (!space) return;
    p->GetMutableDict()
        ->GetOrCreateDictFor("Resources")
        ->GetOrCreateDictFor("ColorSpace")
        ->SetNewFor<CPDF_Reference>(ByteString::Format("CS%u", objnum), d, objnum);
    std::vector<float> tint = {1.0f};
    if (stroke) {
        o->mutable_general_state().SetStrokeAlpha(a);
        o->mutable_color_state().SetStrokeColor(std::move(space), std::move(tint));
    } else {
        o->mutable_general_state().SetFillAlpha(a);
        o->mutable_color_state().SetFillColor(std::move(space), std::move(tint));
    }
    o->SetDirty(true);
}

void PDFKING_PageObj_SetFillSeparation(FPDF_PAGE page, FPDF_PAGEOBJECT obj, uint32_t objnum, float a) {
    SetSeparation(page, obj, objnum, false, a);
}

void PDFKING_PageObj_SetStrokeSeparation(FPDF_PAGE page, FPDF_PAGEOBJECT obj, uint32_t objnum, float a) {
    SetSeparation(page, obj, objnum, true, a);
}

// =============================================================================
// Clip / transparency primitives
// =============================================================================

int PDFKING_PageObj_SetClipRect(FPDF_PAGEOBJECT obj, float left, float bottom, float right, float top) {
    CPDF_PageObject* p = CPDFPageObjectFromFPDFPageObject(obj);
    if (!p) return 0;
    CPDF_Path clipPathObj;
    clipPathObj.Emplace();
    clipPathObj.AppendRect(left, bottom, right, top);
    CPDF_ClipPath& clipPath = p->mutable_clip_path();
    clipPath.Emplace();
    clipPath.AppendPath(clipPathObj, CFX_FillRenderOptions::FillType::kWinding);
    p->SetDirty(true);
    return 1;
}

int PDFKING_XObject_CopyGroup(FPDF_PAGE src_page, FPDF_XOBJECT dest_xobject) {
    CPDF_Page* pPage = CPDFPageFromFPDFPage(src_page);
    if (!pPage) return 0;
    RetainPtr<const CPDF_Dictionary> pPageDict = pPage->GetDict();
    if (!pPageDict) return 0;
    XObjectContext* xobj = XObjectContextFromFPDFXObject(dest_xobject);
    if (!xobj || !xobj->xobject) return 0;
    RetainPtr<CPDF_Dictionary> pDestDict = xobj->xobject->GetMutableDict();
    if (!pDestDict) return 0;

    // Force the PDF spec's default initial graphics state before the copied
    // page content runs. This content used to be an independent page, which
    // always starts from these exact defaults — not whatever state happens
    // to be active in the caller that now invokes it via Do. Most of this
    // is just future-proofing (a Form XObject normally inherits the
    // caller's graphics state, so a source page relying on any of these
    // implicit defaults would otherwise silently pick up the caller's
    // values instead); the colour reset is the one entry that's load-bearing
    // today — sc/SC are colourspace-dependent (variable-arity) operators, so
    // a bare "sc" with no preceding cs, fine under the page-level default,
    // becomes "wrong number of operands" once nested in a Form XObject on
    // strict readers (Acrobat, Ghostscript). w/J/j/M/d are fixed-arity and
    // can't hit that failure mode, but resetting them still guarantees this
    // renders identically to standalone regardless of future caller state.
    RetainPtr<CPDF_Stream> pStream = xobj->xobject;
    static const uint8_t kPrefix[] = "0 g 0 G 1 w 0 J 0 j 10 M [] 0 d\n";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    pdfium::span<const uint8_t> existing = pStream->GetInMemoryRawData();
    uint8_t* combined = static_cast<uint8_t*>(malloc(kPrefixLen + existing.size()));
    memcpy(combined, kPrefix, kPrefixLen);
    memcpy(combined + kPrefixLen, existing.data(), existing.size());
    pStream->SetData(pdfium::span<const uint8_t>(combined, kPrefixLen + existing.size()));
    free(combined);

    RetainPtr<const CPDF_Dictionary> pSrcGroup = pPageDict->GetDictFor("Group");
    if (!pSrcGroup) return 1;
    pDestDict->SetFor("Group", pSrcGroup->Clone());
    return 1;
}

// =============================================================================
// Document primitives (for page export)
// =============================================================================

void* PDFKING_Doc_FromFPDF(FPDF_DOCUMENT doc) {
    return CPDFDocumentFromFPDFDocument(doc);
}

void PDFKING_Doc_SetLastObjNum(void* doc, uint32_t num) {
    static_cast<CPDF_Document*>(doc)->SetLastObjNum(num);
}

uint32_t PDFKING_Doc_GetLastObjNum(void* doc) {
    return static_cast<CPDF_Document*>(doc)->GetLastObjNum();
}

int PDFKING_Doc_ExportPage(void* dest, void* src, uint32_t page_index, int insert_index) {
    CPDF_PageExporter exporter(static_cast<CPDF_Document*>(dest), static_cast<CPDF_Document*>(src));
    std::vector<uint32_t> indices = { page_index };
    return exporter.ExportPages(indices, insert_index) ? 1 : 0;
}

void* PDFKING_Doc_GetObject(void* doc, uint32_t objnum) {
    // parsed if nothing has read it yet
    return static_cast<CPDF_Document*>(doc)->GetOrParseIndirectObject(objnum).Get();
}

// Returns: 0=null/skip, 1=Page, 2=Pages/Catalog(skip), 3=other(write)
int PDFKING_Object_PageTag(void* obj) {
    if (!obj) return 0;
    CPDF_Object* pObj = static_cast<CPDF_Object*>(obj);
    if (!pObj->IsDictionary()) return 3;
    ByteString type = pObj->AsMutableDictionary()->GetByteStringFor("Type");
    if (type == "Page") return 1;
    if (type == "Pages" || type == "Catalog") return 2;
    return 3;
}

void PDFKING_Page_SetParentRef(void* obj, void* doc, uint32_t parent_objnum) {
    CPDF_Dictionary* dict = static_cast<CPDF_Object*>(obj)->AsMutableDictionary();
    if (dict) {
        dict->SetNewFor<CPDF_Reference>("Parent", static_cast<CPDF_Document*>(doc), parent_objnum);
    }
}

// Callback-based archive stream for serializing PDF objects from Rust
// Every token WriteTo writes is a block of its own: they are gathered here
// and handed to `cb` a buffer at a time, as pdfium's FileBufferArchive does
class CallbackArchiveStream : public IFX_ArchiveStream {
public:
    CallbackArchiveStream(PDFKING_WriteFn cb, void* ctx) : cb_(cb), ctx_(ctx) {}
    bool WriteBlock(pdfium::span<const uint8_t> data) override {
        pos_ += data.size();
        if (used_ + data.size() > sizeof(buffer_)) {
            if (!Flush()) return false;
            if (data.size() > sizeof(buffer_)) return Send(data.data(), data.size());
        }
        memcpy(buffer_ + used_, data.data(), data.size());
        used_ += data.size();
        return true;
    }
    FX_FILESIZE CurrentOffset() const override { return static_cast<FX_FILESIZE>(pos_); }
    bool Flush() {
        bool sent = Send(buffer_, used_);
        used_ = 0;
        return sent;
    }
private:
    bool Send(const uint8_t* data, size_t size) {
        return !size || cb_(data, static_cast<uint32_t>(size), ctx_) == static_cast<int>(size);
    }
    PDFKING_WriteFn cb_;
    void* ctx_;
    size_t pos_ = 0;
    size_t used_ = 0;
    // small: it lives on the wasm stack, once per object
    uint8_t buffer_[4096];
};

int PDFKING_Object_Serialize(void* obj, PDFKING_WriteFn cb, void* ctx) {
    if (!obj || !cb) return 0;
    CallbackArchiveStream archive(cb, ctx);
    return static_cast<CPDF_Object*>(obj)->WriteTo(&archive, nullptr) && archive.Flush() ? 1 : 0;
}

// =============================================================================
// Catalog primitives: layers and the shared output intent / XMP
// =============================================================================

// A layer's state rides on its own OCG dict as one number, so whatever copies
// the dict (page export, import) carries it to the destination, where
// PDFKING_Object_LayerFlags reads it back and removes it.
constexpr char kLayerKey[] = "PDFKING_Layer";
constexpr int kLayerOff = 1;
constexpr int kLayerLocked = 2;
constexpr int kLayerHidden = 4;
constexpr int kLayerNoPrint = 8;

// `array` holds a reference to `objnum`, nested arrays included (/Order nests)
static bool HasRef(const CPDF_Array* array, uint32_t objnum, int depth = 0) {
    if (!array || depth > 32) return false;
    for (size_t i = 0; i < array->size(); ++i) {
        RetainPtr<const CPDF_Object> item = array->GetObjectAt(i);
        if (const CPDF_Reference* ref = item->AsReference()) {
            if (ref->GetRefObjNum() == objnum) return true;
        } else if (HasRef(item->AsArray(), objnum, depth + 1)) {
            return true;
        }
    }
    return false;
}

// `config` lists `objnum` under a print /AS entry: its /Usage print state
// applies when printing
static bool PrintUsage(const CPDF_Dictionary* config, uint32_t objnum) {
    RetainPtr<const CPDF_Array> states = config ? config->GetArrayFor("AS") : nullptr;
    if (!states) return false;
    for (size_t i = 0; i < states->size(); ++i) {
        RetainPtr<const CPDF_Dictionary> state = states->GetDictAt(i);
        if (state && state->GetByteStringFor("Event") == "Print" && HasRef(state->GetArrayFor("OCGs").Get(), objnum))
            return true;
    }
    return false;
}

// The OCG left out of print, or printed as everything else: /Usage /Print
static void SetPrint(CPDF_Dictionary* ocg, bool print) {
    if (print) {
        if (RetainPtr<CPDF_Dictionary> usage = ocg->GetMutableDictFor("Usage")) usage->RemoveFor("Print");
        return;
    }
    ocg->GetOrCreateDictFor("Usage")->GetOrCreateDictFor("Print")->SetNewFor<CPDF_Name>("PrintState", "OFF");
}

void PDFKING_Doc_TagLayers(void* doc) {
    CPDF_Document* d = static_cast<CPDF_Document*>(doc);
    const CPDF_Dictionary* root = d->GetRoot();
    RetainPtr<const CPDF_Dictionary> props = root ? root->GetDictFor("OCProperties") : nullptr;
    RetainPtr<const CPDF_Array> ocgs = props ? props->GetArrayFor("OCGs") : nullptr;
    if (!ocgs) return;

    RetainPtr<const CPDF_Dictionary> config = props->GetDictFor("D");
    RetainPtr<const CPDF_Array> on = config ? config->GetArrayFor("ON") : nullptr;
    RetainPtr<const CPDF_Array> off = config ? config->GetArrayFor("OFF") : nullptr;
    RetainPtr<const CPDF_Array> locked = config ? config->GetArrayFor("Locked") : nullptr;
    RetainPtr<const CPDF_Array> order = config ? config->GetArrayFor("Order") : nullptr;
    bool base_off = config && config->GetByteStringFor("BaseState") == "OFF";

    for (size_t i = 0; i < ocgs->size(); ++i) {
        const CPDF_Reference* ref = ocgs->GetObjectAt(i)->AsReference();
        if (!ref) continue;
        uint32_t objnum = ref->GetRefObjNum();
        // parsed if nothing has read it yet: a layer the page does not draw
        RetainPtr<CPDF_Dictionary> ocg = ToDictionary(d->GetOrParseIndirectObject(objnum));
        if (!ocg) continue;
        int flags = 0;
        if (base_off ? !HasRef(on.Get(), objnum) : HasRef(off.Get(), objnum)) flags |= kLayerOff;
        if (HasRef(locked.Get(), objnum)) flags |= kLayerLocked;
        if (order && !HasRef(order.Get(), objnum)) flags |= kLayerHidden;
        RetainPtr<const CPDF_Dictionary> usage = ocg->GetDictFor("Usage");
        RetainPtr<const CPDF_Dictionary> print = usage ? usage->GetDictFor("Print") : nullptr;
        if (print && print->GetByteStringFor("PrintState") == "OFF" && PrintUsage(config.Get(), objnum))
            flags |= kLayerNoPrint;
        ocg->SetNewFor<CPDF_Number>(kLayerKey, flags);
    }
}

// -1 when `obj` is no OCG, else the flags its source tagged it with (none: on)
int PDFKING_Object_LayerFlags(void* obj) {
    CPDF_Dictionary* dict = obj ? static_cast<CPDF_Object*>(obj)->AsMutableDictionary() : nullptr;
    if (!dict || dict->GetByteStringFor("Type") != "OCG") return -1;
    int flags = dict->GetIntegerFor(kLayerKey);
    dict->RemoveFor(kLayerKey);
    return flags;
}

// The OCG's /Name as UTF-8, handed to `cb`
int PDFKING_Object_LayerName(void* obj, PDFKING_WriteFn cb, void* ctx) {
    const CPDF_Dictionary* dict = obj ? static_cast<CPDF_Object*>(obj)->AsDictionary() : nullptr;
    if (!dict) return 0;
    ByteString name = dict->GetUnicodeTextFor("Name").ToUTF8();
    return cb(reinterpret_cast<const uint8_t*>(name.c_str()), name.GetLength(), ctx) >= 0 ? 1 : 0;
}

static void RemapRefs(CPDF_Object* obj, CPDF_Document* doc, const std::map<uint32_t, uint32_t>& map, int depth = 0) {
    if (!obj || depth > 64) return;
    if (CPDF_Reference* ref = obj->AsMutableReference()) {
        auto it = map.find(ref->GetRefObjNum());
        if (it != map.end()) ref->SetRef(doc, it->second);
    } else if (CPDF_Dictionary* dict = obj->AsMutableDictionary()) {
        CPDF_DictionaryLocker locker(dict);
        for (const auto& it : locker) RemapRefs(it.second.Get(), doc, map, depth + 1);
    } else if (CPDF_Array* array = obj->AsMutableArray()) {
        for (size_t i = 0; i < array->size(); ++i) RemapRefs(array->GetMutableObjectAt(i).Get(), doc, map, depth + 1);
    } else if (CPDF_Stream* stream = obj->AsMutableStream()) {
        RemapRefs(stream->GetMutableDict().Get(), doc, map, depth + 1);
    }
}

// Every reference to `from[i]` among objects `first` to `last` made one to
// `to[i]`: the layers a page shares by name with pages before it
void PDFKING_Doc_RemapRefs(void* doc, uint32_t first, uint32_t last, const uint32_t* from, const uint32_t* to, uint32_t count) {
    CPDF_Document* d = static_cast<CPDF_Document*>(doc);
    std::map<uint32_t, uint32_t> map;
    for (uint32_t i = 0; i < count; ++i) map[from[i]] = to[i];
    for (uint32_t n = first; n <= last; ++n) RemapRefs(d->GetMutableIndirectObject(n).Get(), d, map);
}

// The OCG's /Usage as its flags say: left out of print or not
void PDFKING_Object_SetLayerPrint(void* obj, int flags) {
    CPDF_Dictionary* dict = obj ? static_cast<CPDF_Object*>(obj)->AsMutableDictionary() : nullptr;
    if (dict) SetPrint(dict, !(flags & kLayerNoPrint));
}

// The catalog's /OCProperties for these OCGs, written the way the merge's
// tail writes it (see StreamFormat): a print /AS for the ones left out of
// print, their /Usage set on them
void PDFKING_Doc_SetLayers(void* doc, const uint32_t* objnums, const int32_t* flags, uint32_t count) {
    if (!count) return;
    CPDF_Document* d = static_cast<CPDF_Document*>(doc);
    RetainPtr<CPDF_Dictionary> props = d->GetMutableRoot()->SetNewFor<CPDF_Dictionary>("OCProperties");
    RetainPtr<CPDF_Array> ocgs = props->SetNewFor<CPDF_Array>("OCGs");
    RetainPtr<CPDF_Dictionary> config = props->SetNewFor<CPDF_Dictionary>("D");
    RetainPtr<CPDF_Array> order = config->SetNewFor<CPDF_Array>("Order");
    RetainPtr<CPDF_Array> off = config->SetNewFor<CPDF_Array>("OFF");
    RetainPtr<CPDF_Array> locked = config->SetNewFor<CPDF_Array>("Locked");
    // made on the first layer left out of print
    RetainPtr<CPDF_Array> noprint;
    for (uint32_t i = 0; i < count; ++i) {
        ocgs->AppendNew<CPDF_Reference>(d, objnums[i]);
        if (!(flags[i] & kLayerHidden)) order->AppendNew<CPDF_Reference>(d, objnums[i]);
        if (flags[i] & kLayerOff) off->AppendNew<CPDF_Reference>(d, objnums[i]);
        if (flags[i] & kLayerLocked) locked->AppendNew<CPDF_Reference>(d, objnums[i]);
        if (RetainPtr<CPDF_Dictionary> ocg = ToDictionary(d->GetMutableIndirectObject(objnums[i])))
            SetPrint(ocg.Get(), !(flags[i] & kLayerNoPrint));
        if (!(flags[i] & kLayerNoPrint)) continue;
        if (!noprint) {
            RetainPtr<CPDF_Dictionary> state = config->SetNewFor<CPDF_Array>("AS")->AppendNew<CPDF_Dictionary>();
            state->SetNewFor<CPDF_Name>("Event", "Print");
            state->SetNewFor<CPDF_Array>("Category")->AppendNew<CPDF_Name>("Print");
            noprint = state->SetNewFor<CPDF_Array>("OCGs");
        }
        noprint->AppendNew<CPDF_Reference>(d, objnums[i]);
    }
}

// A new OCG, tagged with `flags` as a source document's layers are
uint32_t PDFKING_Doc_NewLayer(void* doc, const char* name, int flags) {
    CPDF_Document* d = static_cast<CPDF_Document*>(doc);
    RetainPtr<CPDF_Dictionary> ocg = d->NewIndirect<CPDF_Dictionary>();
    ocg->SetNewFor<CPDF_Name>("Type", "OCG");
    ocg->SetNewFor<CPDF_String>("Name", WideString::FromUTF8(name).AsStringView());
    ocg->SetNewFor<CPDF_Number>(kLayerKey, flags);
    return ocg->GetObjNum();
}

// `obj` drawn inside the layer: /OC /OC<n> BDC ... EMC, /OC<n> in the page's
// /Properties
void PDFKING_PageObj_SetLayer(FPDF_PAGE page, FPDF_PAGEOBJECT obj, uint32_t objnum) {
    CPDF_Page* p = CPDFPageFromFPDFPage(page);
    CPDF_PageObject* o = CPDFPageObjectFromFPDFPageObject(obj);
    if (!p || !o) return;
    CPDF_Document* d = p->GetDocument();
    RetainPtr<CPDF_Dictionary> ocg = ToDictionary(d->GetMutableIndirectObject(objnum));
    if (!ocg) return;
    ByteString name = ByteString::Format("OC%u", objnum);
    p->GetMutableDict()
        ->GetOrCreateDictFor("Resources")
        ->GetOrCreateDictFor("Properties")
        ->SetNewFor<CPDF_Reference>(name, d, objnum);
    o->GetContentMarks()->AddMarkWithPropertiesHolder("OC", std::move(ocg), name);
    o->SetDirty(true);
}

// Copies source objects into the destination the way page export does: every
// reference they hold is cloned in too.
class CatalogCopier final : public CPDF_PageOrganizer {
public:
    CatalogCopier(CPDF_Document* dest, CPDF_Document* src) : CPDF_PageOrganizer(dest, src) {}
    uint32_t Copy(const CPDF_Object* obj) {
        RetainPtr<CPDF_Object> clone = obj->Clone();
        if (!UpdateReference(clone)) return 0;
        return dest()->AddIndirectObject(std::move(clone));
    }
};

// An uncompressed profile is compressed once, in the source: every page then
// copies it compressed, where the save would compress it again for each one.
static void CompressProfiles(CPDF_Document* src, const CPDF_Array* intents) {
    for (size_t i = 0; i < intents->size(); ++i) {
        RetainPtr<const CPDF_Dictionary> intent = intents->GetDictAt(i);
        RetainPtr<const CPDF_Reference> ref =
            intent ? ToReference(intent->GetObjectFor("DestOutputProfile")) : nullptr;
        RetainPtr<CPDF_Stream> profile =
            ref ? ToStream(src->GetOrParseIndirectObject(ref->GetRefObjNum())) : nullptr;
        if (!profile || profile->HasFilter()) continue;
        auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(profile);
        acc->LoadAllDataRaw();
        profile->TakeData(FlateModule::Encode(acc->GetSpan()));
        profile->GetMutableDict()->SetNewFor<CPDF_Name>("Filter", "FlateDecode");
    }
}

// The source catalog's /OutputIntents and /Metadata, copied in as indirect
// objects and set on the destination catalog; 0 for what the source has not
int PDFKING_Doc_ExportShared(void* dest, void* src, uint32_t* intents, uint32_t* metadata) {
    CPDF_Document* d = static_cast<CPDF_Document*>(dest);
    CPDF_Document* s = static_cast<CPDF_Document*>(src);
    *intents = 0;
    *metadata = 0;
    const CPDF_Dictionary* root = s->GetRoot();
    if (!root) return 1;
    CatalogCopier copier(d, s);
    RetainPtr<CPDF_Dictionary> dest_root = d->GetMutableRoot();
    if (RetainPtr<const CPDF_Object> o = root->GetDirectObjectFor("OutputIntents")) {
        if (const CPDF_Array* array = o->AsArray()) CompressProfiles(s, array);
        *intents = copier.Copy(o.Get());
        if (*intents) dest_root->SetNewFor<CPDF_Reference>("OutputIntents", d, *intents);
    }
    if (RetainPtr<const CPDF_Object> o = root->GetDirectObjectFor("Metadata")) {
        *metadata = copier.Copy(o.Get());
        if (*metadata) dest_root->SetNewFor<CPDF_Reference>("Metadata", d, *metadata);
    }
    return 1;
}

// =============================================================================
// Object primitives: a document's objects as they are read, one at a time —
// what Rust walks, rewrites and writes out (split, the plates)
// =============================================================================

static const CPDF_Object* Object(void* obj) {
    return static_cast<const CPDF_Object*>(obj);
}

// the dictionary `obj` is, or a stream's, mutable
static CPDF_Dictionary* MutableDict(void* obj) {
    CPDF_Object* o = static_cast<CPDF_Object*>(obj);
    if (!o) return nullptr;
    if (CPDF_Stream* stream = o->AsMutableStream()) return stream->GetMutableDict().Get();
    return o->AsMutableDictionary();
}

static ByteStringView Key(const uint8_t* key, uint32_t size) {
    // SAFETY: the caller's slice
    return ByteStringView(UNSAFE_BUFFERS(pdfium::span<const uint8_t>(key, size)));
}

// CPDF_Object::Type: 1 boolean, 2 number, 3 string, 4 name, 5 array,
// 6 dictionary, 7 stream, 8 null, 9 reference; 0 for no object
int PDFKING_Object_Kind(void* obj) {
    return obj ? static_cast<int>(Object(obj)->GetType()) : 0;
}

// what a reference refers to, parsed if nothing has read it yet; any other
// object itself
void* PDFKING_Object_Direct(void* obj) {
    return obj ? const_cast<CPDF_Object*>(Object(obj)->GetDirect().Get()) : nullptr;
}

// the number of the object a reference refers to, or of `obj` itself: 0 for
// a direct one
uint32_t PDFKING_Object_Num(void* obj) {
    if (!obj) return 0;
    const CPDF_Reference* ref = Object(obj)->AsReference();
    return ref ? ref->GetRefObjNum() : Object(obj)->GetObjNum();
}

double PDFKING_Object_Number(void* obj) {
    return obj ? Object(obj)->GetNumber() : 0;
}

// a name's or a string's bytes
int PDFKING_Object_Bytes(void* obj, PDFKING_WriteFn cb, void* ctx) {
    if (!obj) return 0;
    ByteString bytes = Object(obj)->GetString();
    return cb(bytes.unsigned_str(), static_cast<uint32_t>(bytes.GetLength()), ctx) >= 0 ? 1 : 0;
}

// a text string as UTF-8
int PDFKING_Object_Text(void* obj, PDFKING_WriteFn cb, void* ctx) {
    if (!obj) return 0;
    ByteString text = Object(obj)->GetUnicodeText().ToUTF8();
    return cb(text.unsigned_str(), static_cast<uint32_t>(text.GetLength()), ctx) >= 0 ? 1 : 0;
}

// the entry of a dictionary (a stream's, or the one a reference refers to)
// as it holds it: a reference stays one
void* PDFKING_Dict_Get(void* obj, const uint8_t* key, uint32_t size) {
    RetainPtr<const CPDF_Dictionary> dict = obj ? Object(obj)->GetDict() : nullptr;
    return dict ? const_cast<CPDF_Object*>(dict->GetObjectFor(Key(key, size)).Get()) : nullptr;
}

// every entry, in the dictionary's order
void PDFKING_Dict_Each(void* obj, PDFKING_EntryFn cb, void* ctx) {
    RetainPtr<const CPDF_Dictionary> dict = obj ? Object(obj)->GetDict() : nullptr;
    if (!dict) return;
    CPDF_DictionaryLocker locker(dict);
    for (const auto& [key, value] : locker)
        cb(key.unsigned_str(), static_cast<uint32_t>(key.GetLength()), value.Get(), ctx);
}

uint32_t PDFKING_Array_Size(void* obj) {
    const CPDF_Array* array = obj ? Object(obj)->AsArray() : nullptr;
    return array ? static_cast<uint32_t>(array->size()) : 0;
}

void* PDFKING_Array_Get(void* obj, uint32_t index) {
    const CPDF_Array* array = obj ? Object(obj)->AsArray() : nullptr;
    return array && index < array->size() ? const_cast<CPDF_Object*>(array->GetObjectAt(index).Get()) : nullptr;
}

// a stream's data, decoded (`filtered`) or as the file holds it
int PDFKING_Stream_Data(void* obj, int filtered, PDFKING_WriteFn cb, void* ctx) {
    const CPDF_Stream* stream = obj ? Object(obj)->AsStream() : nullptr;
    if (!stream) return 0;
    auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
    if (filtered) acc->LoadAllDataFiltered();
    else acc->LoadAllDataRaw();
    pdfium::span<const uint8_t> data = acc->GetSpan();
    return cb(data.data(), static_cast<uint32_t>(data.size()), ctx) >= 0 ? 1 : 0;
}

static void Refs(const CPDF_Object* obj, PDFKING_RefFn cb, void* ctx, int depth) {
    if (!obj || depth > 64) return;
    if (const CPDF_Reference* ref = obj->AsReference()) {
        cb(ref->GetRefObjNum(), ctx);
    } else if (const CPDF_Array* array = obj->AsArray()) {
        for (size_t i = 0; i < array->size(); ++i) Refs(array->GetObjectAt(i).Get(), cb, ctx, depth + 1);
    } else if (RetainPtr<const CPDF_Dictionary> dict = obj->GetDict()) {
        bool popup = dict->GetNameFor("Subtype") == "Popup";
        CPDF_DictionaryLocker locker(dict);
        for (const auto& [key, value] : locker) {
            if (key == "Prev" || key == "First" || (key == "Parent" && !popup)) continue;
            Refs(value.Get(), cb, ctx, depth + 1);
        }
    }
}

// The objects `obj` refers to, as a page export follows them (see
// CPDF_PageOrganizer::UpdateReference): not through /Prev, /First, nor a
// /Parent other than a popup's — an outline's siblings, a widget's form
// field, the page tree, none of which come along with a page
void PDFKING_Object_Refs(void* obj, PDFKING_RefFn cb, void* ctx) {
    Refs(Object(obj), cb, ctx, 0);
}

void* PDFKING_Doc_PageDict(void* doc, uint32_t page_index) {
    return static_cast<CPDF_Document*>(doc)->GetMutablePageDictionary(static_cast<int>(page_index)).Get();
}

void* PDFKING_Doc_Root(void* doc) {
    return const_cast<CPDF_Dictionary*>(static_cast<CPDF_Document*>(doc)->GetRoot());
}

// whether what is on the layer (an OCG or a membership dictionary) prints
int PDFKING_Doc_LayerPrints(void* doc, void* oc) {
    const CPDF_Dictionary* dict = oc ? Object(oc)->AsDictionary() : nullptr;
    if (!dict) return 1;
    auto print = pdfium::MakeRetain<CPDF_OCContext>(static_cast<CPDF_Document*>(doc), CPDF_OCContext::kPrint);
    return print->CheckOCGDictVisible(dict) ? 1 : 0;
}

// --- written in place: a page cut to what it draws before it is embedded ---

// a new dictionary, the caller's until PDFKING_Object_Release
void* PDFKING_Dict_New() {
    return pdfium::MakeRetain<CPDF_Dictionary>().Leak();
}

void PDFKING_Object_Release(void* obj) {
    RetainPtr<CPDF_Object> owned;
    owned.Unleak(static_cast<CPDF_Object*>(obj));
}

// `value` itself set in the dictionary, which holds it from then on too
void PDFKING_Dict_Set(void* obj, const uint8_t* key, uint32_t size, void* value) {
    if (CPDF_Dictionary* dict = MutableDict(obj))
        dict->SetFor(ByteString(Key(key, size)), pdfium::WrapRetain(static_cast<CPDF_Object*>(value)));
}

// a copy of `value` set in the dictionary
void PDFKING_Dict_SetClone(void* obj, const uint8_t* key, uint32_t size, void* value) {
    if (CPDF_Dictionary* dict = MutableDict(obj)) dict->SetFor(ByteString(Key(key, size)), Object(value)->Clone());
}

// the stream's data, not encoded
void PDFKING_Stream_SetData(void* obj, const uint8_t* data, uint32_t size) {
    CPDF_Stream* stream = obj ? static_cast<CPDF_Object*>(obj)->AsMutableStream() : nullptr;
    // SAFETY: the caller's slice
    if (stream) stream->SetDataAndRemoveFilter(UNSAFE_BUFFERS(pdfium::span<const uint8_t>(data, size)));
}

// --- deflate ---

// Raw deflate at `level`, a segment at a time: each ends byte aligned on a
// sync flush and is not the last block, so segments join into one stream
void* PDFKING_Deflater_New(int level) {
    z_stream* z = new z_stream();
    deflateInit2(z, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    return z;
}

// `data` as one segment, handed to `cb`; its adler32
uint32_t PDFKING_Deflater_Segment(void* deflater, const uint8_t* data, uint32_t size, PDFKING_WriteFn cb, void* ctx) {
    z_stream* z = static_cast<z_stream*>(deflater);
    deflateReset(z);
    DataVector<uint8_t> out(deflateBound(z, size) + 16);
    z->next_in = const_cast<Bytef*>(data);
    z->avail_in = size;
    z->next_out = out.data();
    z->avail_out = static_cast<uInt>(out.size());
    deflate(z, Z_SYNC_FLUSH);
    cb(out.data(), static_cast<uint32_t>(out.size() - z->avail_out), ctx);
    return static_cast<uint32_t>(adler32(1, data, size));
}

void PDFKING_Deflater_Free(void* deflater) {
    z_stream* z = static_cast<z_stream*>(deflater);
    deflateEnd(z);
    delete z;
}


// the page, its content not parsed: its boxes and rotation read, nothing drawn
FPDF_PAGE PDFKING_Page_LoadUnparsed(FPDF_DOCUMENT document, int page_index) {
    CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
    if (!doc || page_index < 0 || page_index >= doc->GetPageCount()) return nullptr;
    RetainPtr<CPDF_Dictionary> dict = doc->GetMutablePageDictionary(page_index);
    if (!CPDF_Page::IsValidPageDictLoose(dict)) return nullptr;
    auto page = pdfium::MakeRetain<CPDF_Page>(doc, std::move(dict));
    page->AddPageImageCache();
    return FPDFPageFromIPDFPage(page.Leak());
}
