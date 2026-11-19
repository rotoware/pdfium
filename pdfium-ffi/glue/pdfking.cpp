// Thin C++ primitives exposing PDFium internal APIs to Rust via FFI.
// Declared in pdfium-ffi (src/lib.rs); built into libpdfium.a by //:pdfium.

#include "public/fpdfview.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_ppo.h"
#include "public/fpdf_transformpage.h"
#include "constants/annotation_flags.h"
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
#include "fpdfsdk/cpdfsdk_helpers.h"
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <unistd.h>

// abseil's raw logging writes through emscripten_errn, a JS import otherwise:
// to stderr instead, libpdfium.a needs no JS beyond libc's
extern "C" void emscripten_errn(const char* text, size_t len) {
  write(2, text, len);
}
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    // --- Color primitives ---
    void PDFKING_PageObj_SetFillRGBA(FPDF_PAGEOBJECT obj, float r, float g, float b, float a);
    void PDFKING_PageObj_SetStrokeRGBA(FPDF_PAGEOBJECT obj, float r, float g, float b, float a);
    void PDFKING_PageObj_SetFillCMYKA(FPDF_PAGEOBJECT obj, float c, float m, float y, float k, float a);
    void PDFKING_PageObj_SetStrokeCMYKA(FPDF_PAGEOBJECT obj, float c, float m, float y, float k, float a);
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
    int PDFKING_Page_Prune(void* doc, uint32_t page_index, int printable, PDFKING_WriteFn cb, void* ctx);

#ifdef __cplusplus
}
#endif

// =============================================================================
// CMYK color primitives
// =============================================================================

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
    RetainPtr<CPDF_Object> obj = static_cast<CPDF_Document*>(doc)->GetMutableIndirectObject(objnum);
    return obj.Get();
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
// Preflight: a page's resources cut to what it draws, the colors of that and
// whether it needs transparency
// =============================================================================

typedef std::set<ByteString> Names;

// What preflight finds in what a page draws: the colors, and whether any of
// it is transparent — drawn flattened (gs -dNOTRANSPARENCY) it would change
struct Found {
    Names colors;
    bool transparency = false;
    // printing: what the layers leave out of print is omitted from the content
    RetainPtr<CPDF_OCContext> print;
};

static const char* ColorantColor(const ByteString& colorant) {
    if (colorant == "Cyan" || colorant == "Magenta" || colorant == "Yellow" || colorant == "Black") return "cmyk";
    return nullptr;
}

static void AddColorant(const ByteString& colorant, Names& colors) {
    if (colorant.IsEmpty() || colorant == "None") return;
    const char* process = ColorantColor(colorant);
    colors.insert(process ? ByteString(process) : colorant);
}

// a color space by name, the inline image abbreviations included
static void AddColorName(const ByteString& name, Names& colors) {
    if (name == "DeviceRGB" || name == "CalRGB" || name == "RGB") colors.insert("rgb");
    if (name == "DeviceCMYK" || name == "CMYK") colors.insert("cmyk");
    if (name == "DeviceGray" || name == "CalGray" || name == "G") colors.insert("gray");
    if (name == "Lab") colors.insert("lab");
}

// The inks a color space paints with: process families as rgb/cmyk/gray/lab,
// spot colorants by their own name.
static void AddColorSpace(RetainPtr<const CPDF_Object> space, Names& colors, int depth = 0) {
    space = space ? space->GetDirect() : nullptr;
    if (!space || depth > 4) return;
    if (const CPDF_Array* array = space->AsArray()) {
        ByteString family = array->GetByteStringAt(0);
        if (family == "ICCBased") {
            RetainPtr<const CPDF_Stream> profile = array->GetStreamAt(1);
            int n = profile ? profile->GetDict()->GetIntegerFor("N") : 0;
            if (n == 1) colors.insert("gray");
            if (n == 3) colors.insert("rgb");
            if (n == 4) colors.insert("cmyk");
        } else if (family == "Indexed" || family == "I" || family == "Pattern") {
            AddColorSpace(array->GetDirectObjectAt(1), colors, depth + 1);
        } else if (family == "Separation") {
            AddColorant(array->GetByteStringAt(1), colors);
        } else if (family == "DeviceN") {
            if (RetainPtr<const CPDF_Array> colorants = array->GetArrayAt(1))
                for (size_t i = 0; i < colorants->size(); ++i) AddColorant(colorants->GetByteStringAt(i), colors);
        } else {
            AddColorSpace(array->GetDirectObjectAt(0), colors, depth + 1);
        }
        return;
    }
    AddColorName(space->GetString(), colors);
}

// What a content owner — the page, a form, a tiling pattern — draws: the
// resource names its content uses and the colors it sets by operator
struct Drawn {
    Names names;
    Names colors;
};

// One pass over a content stream: the /Name operands (the resources it may
// use) and the colors set by operator (rg/k/g, device names, inline images).
// Strings, comments and inline image data are skipped, never read as tokens.
// With `print`, every operator is weighed as it ends: one inside a marked
// section on a layer off print — /OC /<name> BDC ... EMC, the name one of
// `resources`' /Properties — or a Do of an XObject off print, is omitted from the
// stream, operands and all, and what it names or paints is not counted. The
// stream is written back only when something was omitted. A section is omitted whole,
// so whatever state it sets goes with it, as when it does not print.
static void ScanContent(const CPDF_Stream* stream, const CPDF_Dictionary* resources, const CPDF_OCContext* print,
                        Drawn& drawn) {
    if (!stream) return;
    auto acc = pdfium::MakeRetain<CPDF_StreamAcc>(pdfium::WrapRetain(stream));
    acc->LoadAllDataFiltered();
    pdfium::span<const uint8_t> data = acc->GetSpan();
    const size_t n = data.size();
    bool inline_image = false;
    // the operator being read: where its operands start, the names among them
    size_t start_op = 0;
    std::vector<ByteString> names;
    // per marked section open, whether it is omitted: the innermost says
    std::vector<bool> marks;
    std::vector<uint8_t> kept;
    bool omitted_any = false;
    auto printed = [&](const char* category, const ByteString& name, const char* key) {
        RetainPtr<const CPDF_Dictionary> dict = resources ? resources->GetDictFor(category) : nullptr;
        RetainPtr<const CPDF_Dictionary> entry = dict ? dict->GetDictFor(name.AsStringView()) : nullptr;
        if (entry && key) entry = entry->GetDictFor(key);
        return !entry || print->CheckOCGDictVisible(entry.Get());
    };
    // the operator `op` ending at `end`: kept, or omitted
    auto weigh = [&](ByteStringView op, size_t end) {
        bool omit = false;
        if (print) {
            bool hidden = !marks.empty() && marks.back();
            if (op == "BDC" || op == "BMC") {
                omit = hidden || (op == "BDC" && names.size() >= 2 && names[0] == "OC" && !printed("Properties", names[1], nullptr));
                marks.push_back(omit);
            } else if (op == "EMC") {
                omit = hidden;
                if (!marks.empty()) marks.pop_back();
            } else {
                omit = hidden || (op == "Do" && !names.empty() && !printed("XObject", names.back(), "OC"));
            }
        }
        if (!omit) {
            if (op == "rg" || op == "RG") drawn.colors.insert("rgb");
            else if (op == "k" || op == "K") drawn.colors.insert("cmyk");
            else if (op == "g" || op == "G") drawn.colors.insert("gray");
            // an inline image's own abbreviations too
            bool image = op == "EI";
            for (ByteString& name : names) {
                if (name.First(6) == "Device" || (image && (name == "RGB" || name == "CMYK" || name == "G")))
                    AddColorName(name, drawn.colors);
                drawn.names.insert(std::move(name));
            }
            if (print) kept.insert(kept.end(), data.begin() + start_op, data.begin() + end);
        }
        omitted_any |= omit;
        names.clear();
        start_op = end;
    };
    size_t i = 0;
    while (i < n) {
        uint8_t c = data[i];
        if (PDFCharIsWhitespace(c)) {
            ++i;
        } else if (c == '%') {
            while (i < n && data[i] != '\r' && data[i] != '\n') ++i;
        } else if (c == '(') {
            for (int depth = 1; ++i < n && depth;) {
                if (data[i] == '\\') ++i;
                else if (data[i] == '(') ++depth;
                else if (data[i] == ')') --depth;
            }
        } else if (c == '<' && i + 1 < n && data[i + 1] != '<') {
            while (i < n && data[i] != '>') ++i;
            ++i;
        } else if (c == '/') {
            size_t start = ++i;
            while (i < n && !PDFCharIsWhitespace(data[i]) && !PDFCharIsDelimiter(data[i])) ++i;
            ByteString name = PDF_NameDecode(ByteStringView(data.subspan(start, i - start)));
            names.push_back(std::move(name));
        } else if (PDFCharIsDelimiter(c)) {
            ++i;
        } else {
            size_t start = i;
            while (i < n && !PDFCharIsWhitespace(data[i]) && !PDFCharIsDelimiter(data[i])) ++i;
            ByteStringView op(data.subspan(start, i - start));
            // operands: numbers and the keywords that are values
            if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || op == "true" || op == "false" || op == "null")
                continue;
            if (op == "BI") inline_image = true;
            // an inline image is one operator, BI to EI: weighed whole below
            if (inline_image && op != "ID") continue;
            if (op == "ID") {
                // binary up to an EI standing on its own
                inline_image = false;
                for (++i; i + 1 < n; ++i) {
                    if (data[i] == 'E' && data[i + 1] == 'I' && PDFCharIsWhitespace(data[i - 1]) &&
                        (i + 2 == n || PDFCharIsWhitespace(data[i + 2]) || PDFCharIsDelimiter(data[i + 2]))) {
                        i += 2;
                        break;
                    }
                }
                op = "EI";
            }
            weigh(op, i);
        }
    }
    // trailing operands with no operator: kept as they were
    if (print) kept.insert(kept.end(), data.begin() + start_op, data.end());
    for (ByteString& name : names) drawn.names.insert(std::move(name));
    if (omitted_any) const_cast<CPDF_Stream*>(stream)->SetDataAndRemoveFilter(kept);
}

// a page's /Contents: one stream or an array of them
static void ScanContents(const CPDF_Object* contents, const CPDF_Dictionary* resources, const CPDF_OCContext* print,
                         Drawn& drawn) {
    if (!contents) return;
    if (const CPDF_Array* array = contents->AsArray()) {
        for (size_t i = 0; i < array->size(); ++i) ScanContent(array->GetStreamAt(i).Get(), resources, print, drawn);
    } else {
        ScanContent(contents->AsStream(), resources, print, drawn);
    }
}

static const char* const kResourceCategories[] = {
    "XObject", "Font", "ColorSpace", "ExtGState", "Pattern", "Shading", "Properties"};

// The standard blend modes (ISO 32000-2 Tables 134, 135)
static bool BlendMode(const ByteString& mode) {
    for (const char* known : {"Normal", "Compatible", "Multiply", "Screen", "Overlay", "Darken", "Lighten",
                              "ColorDodge", "ColorBurn", "HardLight", "SoftLight", "Difference", "Exclusion",
                              "Hue", "Saturation", "Color", "Luminosity"})
        if (mode == known) return true;
    return false;
}

// An ExtGState, or an annotation, drawing transparent, as gs pdf_check.c
// tells: a soft mask other than /None, an opacity other than 1, a blend mode
// other than Normal or Compatible — of an array, the first standard one, as
// a reader picks it (ISO 32000-2 8.4.5).
static bool TransparentState(const CPDF_Dictionary* dict) {
    if (!dict) return false;
    RetainPtr<const CPDF_Object> smask = dict->GetDirectObjectFor("SMask");
    if (smask && (smask->IsDictionary() || (smask->IsName() && smask->GetString() != "None"))) return true;
    for (const char* opacity : {"CA", "ca"})
        if (dict->KeyExist(opacity) && dict->GetFloatFor(opacity) != 1) return true;
    RetainPtr<const CPDF_Object> blend = dict->GetDirectObjectFor("BM");
    ByteString mode = blend ? blend->GetString() : ByteString();
    if (const CPDF_Array* modes = blend ? blend->AsArray() : nullptr) {
        for (size_t i = 0; i < modes->size() && !BlendMode(mode); ++i) mode = modes->GetByteStringAt(i);
    }
    return BlendMode(mode) && mode != "Normal" && mode != "Compatible";
}

// What the resources `names` uses add to `found` (forms and tiling patterns
// are owners of their own). Colors: color spaces, images, shadings and
// shading patterns. Transparency, as gs pdf_check.c tells: a graphics state
// (a pattern's own too), an image soft-masked — an /SMask, or alpha in its
// JPX data — or a form that is a group. A hard /Mask draws the same
// flattened; so does the page's own group, the page's alone.
static void AddResources(const CPDF_Dictionary* resources, const Names& names, Found& found) {
    auto used = [&](const char* category, auto fn) {
        RetainPtr<const CPDF_Dictionary> dict = resources->GetDictFor(category);
        if (!dict) return;
        for (const ByteString& key : dict->GetKeys())
            if (names.count(key)) fn(dict->GetDirectObjectFor(key.AsStringView()));
    };
    auto shading = [&](RetainPtr<const CPDF_Object> object) {
        const CPDF_Dictionary* dict = object ? object->GetDict() : nullptr;
        if (dict) AddColorSpace(dict->GetObjectFor("ColorSpace"), found.colors);
    };
    used("ColorSpace", [&](RetainPtr<const CPDF_Object> space) { AddColorSpace(space, found.colors); });
    used("Shading", shading);
    used("ExtGState", [&](RetainPtr<const CPDF_Object> state) {
        found.transparency |= state && TransparentState(state->GetDict());
    });
    used("Pattern", [&](RetainPtr<const CPDF_Object> pattern) {
        const CPDF_Dictionary* dict = pattern ? pattern->GetDict() : nullptr;
        if (!dict) return;
        if (dict->GetIntegerFor("PatternType") == 2) shading(dict->GetDirectObjectFor("Shading"));
        found.transparency |= TransparentState(dict->GetDictFor("ExtGState").Get());
    });
    used("XObject", [&](RetainPtr<const CPDF_Object> xobject) {
        const CPDF_Dictionary* dict = xobject ? xobject->GetDict() : nullptr;
        if (!dict) return;
        ByteString subtype = dict->GetNameFor("Subtype");
        if (subtype == "Form") found.transparency |= !!dict->GetDictFor("Group");
        if (subtype != "Image") return;
        // stencil masks paint with the fill color, set by operator
        if (!dict->GetBooleanFor("ImageMask", false)) AddColorSpace(dict->GetObjectFor("ColorSpace"), found.colors);
        found.transparency |= dict->KeyExist("SMask") || dict->GetFloatFor("SMaskInData") != 0;
    });
}

// `resources` keeping only what `names` draws, or null to keep it as is
// (Type3 fonts without their own resources draw from these).
static RetainPtr<CPDF_Dictionary> PrunedResources(const CPDF_Dictionary* resources, const Names& names) {
    if (RetainPtr<const CPDF_Dictionary> fonts = resources->GetDictFor("Font")) {
        for (const ByteString& key : fonts->GetKeys()) {
            RetainPtr<const CPDF_Dictionary> font = fonts->GetDictFor(key.AsStringView());
            if (names.count(key) && font && font->GetNameFor("Subtype") == "Type3" && !font->KeyExist("Resources"))
                return nullptr;
        }
    }

    auto pruned = pdfium::MakeRetain<CPDF_Dictionary>();
    CPDF_DictionaryLocker locker(resources);
    for (const auto& [category, value] : locker) {
        bool prunable = false;
        for (const char* c : kResourceCategories) prunable |= category == c;
        RetainPtr<const CPDF_Dictionary> dict = prunable ? resources->GetDictFor(category.AsStringView()) : nullptr;
        if (!dict) {
            pruned->SetFor(category, value->Clone());
            continue;
        }
        auto kept = pruned->SetNewFor<CPDF_Dictionary>(category);
        CPDF_DictionaryLocker entries(dict);
        for (const auto& [name, entry] : entries) {
            // /DefaultRGB, /DefaultCMYK, /DefaultGray apply to every device color
            if (names.count(name) || (category == "ColorSpace" && name.First(7) == "Default"))
                kept->SetFor(name, entry->Clone());
        }
    }
    return pruned;
}

static void PruneOwner(CPDF_Dictionary* owner, const CPDF_Object* contents, RetainPtr<const CPDF_Dictionary> resources,
                       Found& found, std::set<const CPDF_Object*>& seen);

// `contents` scanned into `drawn`, and through `resources` the forms,
// tiling patterns and Type3 fonts it draws, each once (`seen`): one with
// resources of its own is an owner in turn (a Type3 font's left whole); one
// without draws from `resources` and is scanned into this very `drawn`,
// which may name more.
static void Walk(const CPDF_Object* contents, const CPDF_Dictionary* resources, Drawn& drawn, Found& found,
                 std::set<const CPDF_Object*>& seen) {
    ScanContents(contents, resources, found.print.Get(), drawn);
    if (!resources) return;
    for (bool grew = true; grew;) {
        grew = false;
        for (const char* category : {"XObject", "Pattern"}) {
            RetainPtr<const CPDF_Dictionary> dict = resources->GetDictFor(category);
            if (!dict) continue;
            CPDF_DictionaryLocker locker(dict);
            for (const auto& [name, entry] : locker) {
                if (!drawn.names.count(name)) continue;
                RetainPtr<CPDF_Stream> stream = ToStream(entry->GetMutableDirect());
                if (!stream || !seen.insert(stream.Get()).second) continue;
                RetainPtr<CPDF_Dictionary> dict = stream->GetMutableDict();
                // images are streams too: only forms and tiling patterns have content
                bool content = category == ByteStringView("XObject") ? dict->GetNameFor("Subtype") == "Form"
                                                                      : dict->GetIntegerFor("PatternType") == 1;
                if (!content) continue;
                if (RetainPtr<const CPDF_Dictionary> own = dict->GetDictFor("Resources")) {
                    PruneOwner(dict.Get(), stream.Get(), own, found, seen);
                    continue;
                }
                size_t before = drawn.names.size();
                Walk(stream.Get(), resources, drawn, found, seen);
                grew |= drawn.names.size() != before;
            }
        }
        // Type3 glyphs are content too, every one: drawn from the font's own
        // resources, kept whole, or from these
        RetainPtr<const CPDF_Dictionary> fonts = resources->GetDictFor("Font");
        if (!fonts) continue;
        CPDF_DictionaryLocker locker(fonts);
        for (const auto& [name, entry] : locker) {
            if (!drawn.names.count(name)) continue;
            RetainPtr<const CPDF_Dictionary> font = ToDictionary(entry->GetDirect());
            if (!font || font->GetNameFor("Subtype") != "Type3" || !seen.insert(font.Get()).second) continue;
            RetainPtr<const CPDF_Dictionary> procs = font->GetDictFor("CharProcs");
            if (!procs) continue;
            RetainPtr<const CPDF_Dictionary> own = font->GetDictFor("Resources");
            Drawn glyphs;
            size_t before = drawn.names.size();
            CPDF_DictionaryLocker glyph_locker(procs);
            for (const auto& [glyph, proc] : glyph_locker)
                Walk(proc->GetDirect().Get(), own ? own.Get() : resources, own ? glyphs : drawn, found, seen);
            grew |= drawn.names.size() != before;
            if (!own) continue;
            found.colors.insert(glyphs.colors.begin(), glyphs.colors.end());
            AddResources(own.Get(), glyphs.names, found);
        }
    }
}

// `owner`'s resources cut to what its content draws — a fresh dict on the
// owner, the shared one untouched — and the colors and transparency of what
// is left
static void PruneOwner(CPDF_Dictionary* owner, const CPDF_Object* contents, RetainPtr<const CPDF_Dictionary> resources,
                       Found& found, std::set<const CPDF_Object*>& seen) {
    Drawn drawn;
    Walk(contents, resources.Get(), drawn, found, seen);
    found.colors.insert(drawn.colors.begin(), drawn.colors.end());
    if (!resources) return;
    AddResources(resources.Get(), drawn.names, found);
    if (RetainPtr<CPDF_Dictionary> pruned = PrunedResources(resources.Get(), drawn.names))
        owner->SetFor("Resources", std::move(pruned));
}

// The page walked once, before anything copies it: its resources and those
// of every form and tiling pattern it draws — and of the appearances of the
// annotations it prints — cut to what is used, so an export never copies
// the rest, and the colors drawn with — rgb/cmyk/gray/lab, spot colorants by
// name — handed to `cb` once each, \0 terminated. 1 when any of it is
// transparent.
int PDFKING_Page_Prune(void* doc, uint32_t page_index, int printable, PDFKING_WriteFn cb, void* ctx) {
    CPDF_Document* d = static_cast<CPDF_Document*>(doc);
    RetainPtr<CPDF_Dictionary> page = d->GetMutablePageDictionary(static_cast<int>(page_index));
    if (!page) return 0;
    // resources may be inherited from the page tree
    RetainPtr<const CPDF_Dictionary> resources;
    RetainPtr<const CPDF_Dictionary> node = page;
    for (int depth = 0; node && !resources && depth < 64; ++depth, node = node->GetDictFor("Parent"))
        resources = node->GetDictFor("Resources");
    Found found;
    if (printable) found.print = pdfium::MakeRetain<CPDF_OCContext>(d, CPDF_OCContext::kPrint);
    std::set<const CPDF_Object*> seen;
    PruneOwner(page.Get(), page->GetDirectObjectFor("Contents").Get(), resources, found, seen);
    // printed annotations: their own opacity, and their normal appearance —
    // the one their /AS picks when there are several
    if (RetainPtr<CPDF_Array> annots = page->GetMutableArrayFor("Annots")) {
        for (size_t i = 0; i < annots->size(); ++i) {
            RetainPtr<CPDF_Dictionary> annot = annots->GetMutableDictAt(i);
            if (!annot) continue;
            uint32_t flags = static_cast<uint32_t>(annot->GetIntegerFor("F"));
            ByteString subtype = annot->GetNameFor("Subtype");
            // as gs prints: 3D ones even without the Print flag
            bool printed = (flags & pdfium::annotation_flags::kPrint) || subtype == "3D";
            if (!printed || (flags & pdfium::annotation_flags::kHidden)) continue;
            // gs draws a Highlight with no appearance multiplied, and counts
            // every one transparent
            found.transparency |= subtype == "Highlight" || TransparentState(annot.Get());
            RetainPtr<CPDF_Dictionary> ap = annot->GetMutableDictFor("AP");
            RetainPtr<CPDF_Object> normal = ap ? ap->GetMutableDirectObjectFor("N") : nullptr;
            if (normal && normal->IsDictionary())
                normal = normal->AsMutableDictionary()->GetMutableDirectObjectFor(annot->GetNameFor("AS").AsStringView());
            RetainPtr<CPDF_Stream> stream = ToStream(normal);
            if (!stream || !seen.insert(stream.Get()).second) continue;
            RetainPtr<CPDF_Dictionary> dict = stream->GetMutableDict();
            PruneOwner(dict.Get(), stream.Get(), dict->GetDictFor("Resources"), found, seen);
        }
    }
    for (const ByteString& color : found.colors)
        cb(color.unsigned_str(), static_cast<uint32_t>(color.GetLength() + 1), ctx);
    return found.transparency;
}
