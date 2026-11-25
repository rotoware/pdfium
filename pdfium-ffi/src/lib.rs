#![allow(non_camel_case_types, non_snake_case, dead_code)]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;
use core::ffi::{c_char, c_double, c_float, c_int, c_uint, c_ulong, c_void};

// --- Opaque pointer types ---

pub type FPDF_DOCUMENT = *mut c_void;
pub type FPDF_PAGE = *mut c_void;
pub type FPDF_PAGEOBJECT = *mut c_void;
pub type FPDF_XOBJECT = *mut c_void;
pub type FPDF_BITMAP = *mut c_void;
pub type FPDF_AVAIL = *mut c_void;

// --- Callback structs (needed by reader/save) ---

#[repr(C)]
pub struct FS_RECTF {
    pub left: c_float,
    pub top: c_float,
    pub right: c_float,
    pub bottom: c_float,
}

#[repr(C)]
pub struct FS_MATRIX {
    pub a: c_float,
    pub b: c_float,
    pub c: c_float,
    pub d: c_float,
    pub e: c_float,
    pub f: c_float,
}

#[repr(C)]
pub struct FPDF_FILEWRITE {
    pub version: c_int,
    pub WriteBlock: Option<
        unsafe extern "C" fn(
            pThis: *mut FPDF_FILEWRITE,
            pData: *const c_void,
            size: c_ulong,
        ) -> c_int,
    >,
}

#[repr(C)]
pub struct FPDF_FILEACCESS {
    pub m_FileLen: c_ulong,
    pub m_GetBlock: Option<
        unsafe extern "C" fn(
            param: *mut c_void,
            position: c_ulong,
            pBuf: *mut u8,
            size: c_ulong,
        ) -> c_int,
    >,
    pub m_pData: *mut c_void,
}

#[repr(C)]
pub struct FX_FILEAVAIL {
    pub version: c_int,
    pub IsDataAvail: Option<
        unsafe extern "C" fn(pThis: *mut FX_FILEAVAIL, offset: c_ulong, size: c_ulong) -> c_int,
    >,
}

#[repr(C)]
pub struct FX_DOWNLOADHINTS {
    pub version: c_int,
    pub AddSegment:
        Option<unsafe extern "C" fn(pThis: *mut FX_DOWNLOADHINTS, offset: c_ulong, size: c_ulong)>,
}

// --- Raw extern declarations (private) ---

mod raw {
    type WriteFn = unsafe extern "C" fn(*const u8, u32, *mut core::ffi::c_void) -> core::ffi::c_int;
    type EntryFn =
        unsafe extern "C" fn(*const u8, u32, *mut core::ffi::c_void, *mut core::ffi::c_void) -> core::ffi::c_int;
    use super::*;
    extern "C" {
        // pdfium
        pub fn FPDF_InitLibrary();
        pub fn FPDF_SetSystemFontInfo(font_info: *mut c_void);
        pub fn FPDF_DestroyLibrary();
        pub fn FPDF_GetLastError() -> c_ulong;
        pub fn FPDF_CreateNewDocument() -> FPDF_DOCUMENT;
        pub fn FPDF_CloseDocument(document: FPDF_DOCUMENT);
        pub fn FPDF_LoadMemDocument(
            data_buf: *const u8,
            size: c_int,
            password: *const c_char,
        ) -> FPDF_DOCUMENT;
        pub fn FPDF_LoadCustomDocument(
            pFileAccess: *mut FPDF_FILEACCESS,
            password: *const c_char,
        ) -> FPDF_DOCUMENT;
        pub fn FPDF_GetPageCount(document: FPDF_DOCUMENT) -> c_int;
        pub fn FPDF_ImportPages(
            dest_doc: FPDF_DOCUMENT,
            src_doc: FPDF_DOCUMENT,
            pagerange: *const c_char,
            index: c_int,
        ) -> c_int;
        pub fn FPDF_SaveAsCopy(
            document: FPDF_DOCUMENT,
            pFileWrite: *mut FPDF_FILEWRITE,
            flags: c_ulong,
        ) -> c_int;
        pub fn FPDF_LoadPage(document: FPDF_DOCUMENT, page_index: c_int) -> FPDF_PAGE;
        pub fn FPDF_ClosePage(page: FPDF_PAGE);
        pub fn FPDFPage_New(
            document: FPDF_DOCUMENT,
            page_index: c_int,
            width: c_double,
            height: c_double,
        ) -> FPDF_PAGE;
        pub fn FPDF_GetPageHeightF(page: FPDF_PAGE) -> c_float;
        pub fn FPDFPage_GetMediaBox(
            page: FPDF_PAGE,
            left: *mut c_float,
            bottom: *mut c_float,
            right: *mut c_float,
            top: *mut c_float,
        ) -> c_int;
        pub fn FPDFPage_GetTrimBox(
            page: FPDF_PAGE,
            left: *mut c_float,
            bottom: *mut c_float,
            right: *mut c_float,
            top: *mut c_float,
        ) -> c_int;
        pub fn FPDF_GetPageBoundingBox(page: FPDF_PAGE, rect: *mut FS_RECTF) -> c_int;
        pub fn FPDFPage_GetRotation(page: FPDF_PAGE) -> c_int;
        pub fn FPDF_RenderPageBitmapWithMatrix(
            bitmap: FPDF_BITMAP,
            page: FPDF_PAGE,
            matrix: *const FS_MATRIX,
            clipping: *const FS_RECTF,
            flags: c_int,
        );
        pub fn FPDFPage_SetMediaBox(
            page: FPDF_PAGE,
            left: c_float,
            bottom: c_float,
            right: c_float,
            top: c_float,
        );
        pub fn FPDFPage_SetTrimBox(
            page: FPDF_PAGE,
            left: c_float,
            bottom: c_float,
            right: c_float,
            top: c_float,
        );
        pub fn FPDFPage_GenerateContent(page: FPDF_PAGE) -> c_int;
        pub fn FPDFPage_InsertObject(page: FPDF_PAGE, page_obj: FPDF_PAGEOBJECT) -> c_int;
        pub fn FPDFPageObj_NewImageObj(document: FPDF_DOCUMENT) -> FPDF_PAGEOBJECT;
        pub fn FPDFImageObj_LoadJpegFileInline(
            pages: *mut FPDF_PAGE,
            count: c_int,
            image_object: FPDF_PAGEOBJECT,
            file_access: *mut FPDF_FILEACCESS,
        ) -> c_int;
        pub fn FPDFImageObj_GetImagePixelSize(
            image_object: FPDF_PAGEOBJECT,
            width: *mut c_uint,
            height: *mut c_uint,
        ) -> c_int;
        pub fn FPDFImageObj_SetBitmap(
            pages: *mut FPDF_PAGE,
            count: c_int,
            image_object: FPDF_PAGEOBJECT,
            bitmap: FPDF_BITMAP,
        ) -> c_int;
        pub fn FPDFBitmap_CreateEx(
            width: c_int,
            height: c_int,
            format: c_int,
            first_scan: *mut c_void,
            stride: c_int,
        ) -> FPDF_BITMAP;
        pub fn FPDFBitmap_Destroy(bitmap: FPDF_BITMAP);
        pub fn FPDFPageObj_Transform(
            page_object: FPDF_PAGEOBJECT,
            a: c_double,
            b: c_double,
            c: c_double,
            d: c_double,
            e: c_double,
            f: c_double,
        );
        pub fn FPDFPageObj_GetBounds(
            page_object: FPDF_PAGEOBJECT,
            left: *mut c_float,
            bottom: *mut c_float,
            right: *mut c_float,
            top: *mut c_float,
        ) -> c_int;
        pub fn FPDFPageObj_SetStrokeWidth(page_object: FPDF_PAGEOBJECT, width: c_float) -> c_int;
        pub fn FPDFPageObj_CreateNewRect(
            x: c_float,
            y: c_float,
            w: c_float,
            h: c_float,
        ) -> FPDF_PAGEOBJECT;
        pub fn FPDFPageObj_CreateNewPath(x: c_float, y: c_float) -> FPDF_PAGEOBJECT;
        pub fn FPDFPath_BezierTo(
            path: FPDF_PAGEOBJECT,
            x1: c_float,
            y1: c_float,
            x2: c_float,
            y2: c_float,
            x3: c_float,
            y3: c_float,
        ) -> c_int;
        pub fn FPDFPath_Close(path: FPDF_PAGEOBJECT) -> c_int;
        pub fn FPDFPath_SetDrawMode(path: FPDF_PAGEOBJECT, fillmode: c_int, stroke: c_int)
            -> c_int;
        pub fn FPDFPageObj_NewTextObj(
            document: FPDF_DOCUMENT,
            font: *const c_char,
            font_size: c_float,
        ) -> FPDF_PAGEOBJECT;
        pub fn FPDFText_SetText(text_object: FPDF_PAGEOBJECT, text: *const u16) -> c_int;
        pub fn FPDF_NewXObjectFromPage(
            dest_doc: FPDF_DOCUMENT,
            src_doc: FPDF_DOCUMENT,
            src_page_index: c_int,
        ) -> FPDF_XOBJECT;
        pub fn FPDF_CloseXObject(xobject: FPDF_XOBJECT);
        pub fn FPDF_NewFormObjectFromXObject(xobject: FPDF_XOBJECT) -> FPDF_PAGEOBJECT;
        pub fn FPDFAvail_Create(
            pFileAvail: *mut FX_FILEAVAIL,
            pFileAccess: *mut FPDF_FILEACCESS,
        ) -> FPDF_AVAIL;
        pub fn FPDFAvail_Destroy(avail: FPDF_AVAIL);
        pub fn FPDFAvail_IsDocAvail(avail: FPDF_AVAIL, hints: *mut FX_DOWNLOADHINTS) -> c_int;
        pub fn FPDFAvail_GetDocument(avail: FPDF_AVAIL, password: *const c_char) -> FPDF_DOCUMENT;
        pub fn FPDFAvail_IsPageAvail(
            avail: FPDF_AVAIL,
            page_index: c_int,
            hints: *mut FX_DOWNLOADHINTS,
        ) -> c_int;
        // pdfking
        pub fn PDFKING_CmykToRgb(cmyk: *const u8, rgb: *mut u8, pixels: c_uint);
        pub fn PDFKING_PageObj_SetFillRGBA(
            obj: FPDF_PAGEOBJECT,
            r: c_float,
            g: c_float,
            b: c_float,
            a: c_float,
        );
        pub fn PDFKING_PageObj_SetStrokeRGBA(
            obj: FPDF_PAGEOBJECT,
            r: c_float,
            g: c_float,
            b: c_float,
            a: c_float,
        );
        pub fn PDFKING_PageObj_SetFillCMYKA(
            obj: FPDF_PAGEOBJECT,
            c: c_float,
            m: c_float,
            y: c_float,
            k: c_float,
            a: c_float,
        );
        pub fn PDFKING_PageObj_SetStrokeCMYKA(
            obj: FPDF_PAGEOBJECT,
            c: c_float,
            m: c_float,
            y: c_float,
            k: c_float,
            a: c_float,
        );
        pub fn PDFKING_PageObj_SetClipRect(
            obj: FPDF_PAGEOBJECT,
            left: c_float,
            bottom: c_float,
            right: c_float,
            top: c_float,
        ) -> c_int;
        pub fn PDFKING_XObject_CopyGroup(src_page: FPDF_PAGE, dest_xobject: FPDF_XOBJECT) -> c_int;
        pub fn PDFKING_Doc_FromFPDF(doc: FPDF_DOCUMENT) -> *mut c_void;
        pub fn PDFKING_Doc_SetLastObjNum(doc: *mut c_void, num: c_uint);
        pub fn PDFKING_Doc_GetLastObjNum(doc: *mut c_void) -> c_uint;
        pub fn PDFKING_Doc_ExportPage(
            dest: *mut c_void,
            src: *mut c_void,
            page_index: c_uint,
            insert_index: c_int,
        ) -> c_int;
        pub fn PDFKING_Doc_GetObject(doc: *mut c_void, objnum: c_uint) -> *mut c_void;
        pub fn PDFKING_Object_PageTag(obj: *mut c_void) -> c_int;
        pub fn PDFKING_Page_SetParentRef(obj: *mut c_void, doc: *mut c_void, parent_objnum: c_uint);
        pub fn PDFKING_Object_Serialize(
            obj: *mut c_void,
            cb: unsafe extern "C" fn(*const u8, u32, *mut c_void) -> c_int,
            ctx: *mut c_void,
        ) -> c_int;
        pub fn PDFKING_Doc_TagLayers(doc: *mut c_void);
        pub fn PDFKING_Object_LayerFlags(obj: *mut c_void) -> c_int;
        pub fn PDFKING_Object_LayerName(
            obj: *mut c_void,
            cb: unsafe extern "C" fn(*const u8, u32, *mut c_void) -> c_int,
            ctx: *mut c_void,
        ) -> c_int;
        pub fn PDFKING_Doc_RemapRefs(
            doc: *mut c_void,
            first: c_uint,
            last: c_uint,
            from: *const c_uint,
            to: *const c_uint,
            count: c_uint,
        );
        pub fn PDFKING_Object_SetLayerPrint(obj: *mut c_void, flags: c_int);
        pub fn PDFKING_Doc_SetLayers(
            doc: *mut c_void,
            objnums: *const c_uint,
            flags: *const c_int,
            count: c_uint,
        );
        pub fn PDFKING_Doc_NewLayer(doc: *mut c_void, name: *const c_char, flags: c_int) -> c_uint;
        pub fn PDFKING_Doc_NewSeparation(
            doc: *mut c_void,
            name: *const c_char,
            c: c_float,
            m: c_float,
            y: c_float,
            k: c_float,
        ) -> c_uint;
        pub fn PDFKING_PageObj_SetFillSeparation(page: FPDF_PAGE, obj: FPDF_PAGEOBJECT, objnum: c_uint, a: c_float);
        pub fn PDFKING_PageObj_SetStrokeSeparation(page: FPDF_PAGE, obj: FPDF_PAGEOBJECT, objnum: c_uint, a: c_float);
        pub fn PDFKING_PageObj_SetLayer(page: FPDF_PAGE, obj: FPDF_PAGEOBJECT, objnum: c_uint);
        pub fn PDFKING_Doc_ExportShared(
            dest: *mut c_void,
            src: *mut c_void,
            intents: *mut c_uint,
            metadata: *mut c_uint,
        ) -> c_int;
        pub fn PDFKING_Page_LoadUnparsed(document: FPDF_DOCUMENT, page_index: c_int) -> FPDF_PAGE;
        pub fn PDFKING_FormObj_FromXObject(xobject: FPDF_XOBJECT) -> FPDF_PAGEOBJECT;
        // object primitives
        pub fn PDFKING_Object_Kind(obj: *mut c_void) -> c_int;
        pub fn PDFKING_Object_Direct(obj: *mut c_void) -> *mut c_void;
        pub fn PDFKING_Object_Num(obj: *mut c_void) -> c_uint;
        pub fn PDFKING_Object_Number(obj: *mut c_void) -> c_double;
        pub fn PDFKING_Object_Bytes(obj: *mut c_void, cb: WriteFn, ctx: *mut c_void) -> c_int;
        pub fn PDFKING_Object_Text(obj: *mut c_void, cb: WriteFn, ctx: *mut c_void) -> c_int;
        pub fn PDFKING_Dict_Get(obj: *mut c_void, key: *const u8, size: c_uint) -> *mut c_void;
        pub fn PDFKING_Dict_Each(obj: *mut c_void, cb: EntryFn, ctx: *mut c_void);
        pub fn PDFKING_Array_Size(obj: *mut c_void) -> c_uint;
        pub fn PDFKING_Array_Get(obj: *mut c_void, index: c_uint) -> *mut c_void;
        pub fn PDFKING_Stream_Data(obj: *mut c_void, filtered: c_int, cb: WriteFn, ctx: *mut c_void) -> c_int;
        pub fn PDFKING_Object_Refs(obj: *mut c_void, cb: unsafe extern "C" fn(c_uint, *mut c_void), ctx: *mut c_void);
        pub fn PDFKING_Doc_PageDict(doc: *mut c_void, page_index: c_uint) -> *mut c_void;
        pub fn PDFKING_Doc_Root(doc: *mut c_void) -> *mut c_void;
        pub fn PDFKING_Doc_LayerPrints(doc: *mut c_void, oc: *mut c_void) -> c_int;
        pub fn PDFKING_Dict_New() -> *mut c_void;
        pub fn PDFKING_Object_Release(obj: *mut c_void);
        pub fn PDFKING_Dict_Set(obj: *mut c_void, key: *const u8, size: c_uint, value: *mut c_void);
        pub fn PDFKING_Dict_SetClone(obj: *mut c_void, key: *const u8, size: c_uint, value: *mut c_void);
        pub fn PDFKING_Stream_SetData(obj: *mut c_void, data: *const u8, size: c_uint);
        pub fn PDFKING_Deflater_New(level: c_int) -> *mut c_void;
        pub fn PDFKING_Deflater_Segment(
            deflater: *mut c_void,
            data: *const u8,
            size: c_uint,
            cb: WriteFn,
            ctx: *mut c_void,
        ) -> c_uint;
        pub fn PDFKING_Deflater_Free(deflater: *mut c_void);
        // lcms2
        pub fn cmsCreate_sRGBProfile() -> *mut c_void;
        pub fn cmsOpenProfileFromMem(data: *const c_void, size: c_uint) -> *mut c_void;
        pub fn cmsCreateProofingTransform(
            input: *mut c_void,
            input_format: c_uint,
            output: *mut c_void,
            output_format: c_uint,
            proofing: *mut c_void,
            intent: c_uint,
            proofing_intent: c_uint,
            flags: c_uint,
        ) -> *mut c_void;
        pub fn cmsDoTransform(
            transform: *mut c_void,
            input: *const c_void,
            output: *mut c_void,
            size: c_uint,
        );
        pub fn cmsDeleteTransform(transform: *mut c_void);
        pub fn cmsCloseProfile(profile: *mut c_void) -> c_int;
    }
}

// --- Error helper ---

pub fn last_error<T>(ctx: &str) -> Result<T, String> {
    let code = unsafe { raw::FPDF_GetLastError() };
    let reason = match code {
        1 => "unknown error",
        2 => "file not found or could not be opened",
        3 => "file not in PDF format or corrupted",
        4 => "password required or incorrect password",
        5 => "unsupported security scheme",
        6 => "page not found or content error",
        7 => "load XFA error",
        8 => "layout XFA error",
        _ => "success or unknown code",
    };
    Err(format!("{ctx}: {reason} (code {code})"))
}

// --- Library lifecycle ---

pub fn FPDF_InitLibrary() {
    unsafe { raw::FPDF_InitLibrary() }
}
/// `null` drops the platform's font lookup: only the built-in fonts remain.
pub fn FPDF_SetSystemFontInfo(font_info: *mut c_void) {
    unsafe { raw::FPDF_SetSystemFontInfo(font_info) }
}
pub fn FPDF_DestroyLibrary() {
    unsafe { raw::FPDF_DestroyLibrary() }
}

// --- Document ---

pub fn FPDF_CreateNewDocument() -> Result<FPDF_DOCUMENT, String> {
    let doc = unsafe { raw::FPDF_CreateNewDocument() };
    if doc.is_null() {
        last_error("create document")
    } else {
        Ok(doc)
    }
}

pub fn FPDF_CloseDocument(doc: FPDF_DOCUMENT) {
    unsafe { raw::FPDF_CloseDocument(doc) }
}

pub fn FPDF_LoadMemDocument(data: &[u8]) -> Result<FPDF_DOCUMENT, String> {
    let doc =
        unsafe { raw::FPDF_LoadMemDocument(data.as_ptr(), data.len() as c_int, core::ptr::null()) };
    if doc.is_null() {
        last_error("load document")
    } else {
        Ok(doc)
    }
}

// `file_access` is read for as long as the document is open: its owner keeps
// it alive until FPDF_CloseDocument.
pub fn FPDF_LoadCustomDocument(file_access: &mut FPDF_FILEACCESS) -> Result<FPDF_DOCUMENT, String> {
    let doc = unsafe { raw::FPDF_LoadCustomDocument(file_access, core::ptr::null()) };
    if doc.is_null() {
        last_error("load document")
    } else {
        Ok(doc)
    }
}

pub fn FPDF_GetPageCount(doc: FPDF_DOCUMENT) -> i32 {
    unsafe { raw::FPDF_GetPageCount(doc) as i32 }
}

pub fn FPDF_ImportPages(
    dest: FPDF_DOCUMENT,
    src: FPDF_DOCUMENT,
    range: *const c_char,
    index: i32,
) -> Result<(), String> {
    if unsafe { raw::FPDF_ImportPages(dest, src, range, index as c_int) } == 0 {
        last_error("import pages")
    } else {
        Ok(())
    }
}

pub fn FPDF_SaveAsCopy(doc: FPDF_DOCUMENT, writer: &mut FPDF_FILEWRITE) -> Result<(), String> {
    if unsafe { raw::FPDF_SaveAsCopy(doc, writer, 0 as c_ulong) } == 0 {
        last_error("save document")
    } else {
        Ok(())
    }
}

// --- Page ---

pub fn FPDF_LoadPage(doc: FPDF_DOCUMENT, index: i32) -> Result<FPDF_PAGE, String> {
    let page = unsafe { raw::FPDF_LoadPage(doc, index as c_int) };
    if page.is_null() {
        last_error("load page")
    } else {
        Ok(page)
    }
}

/// A form object placed by its stream alone: its content not parsed, its
/// box empty until a page read back parses it.
pub fn PDFKING_FormObj_FromXObject(xobject: FPDF_XOBJECT) -> Result<FPDF_PAGEOBJECT, String> {
    let obj = unsafe { raw::PDFKING_FormObj_FromXObject(xobject) };
    if obj.is_null() {
        last_error("create form object")
    } else {
        Ok(obj)
    }
}

/// The page with its content not parsed: for its boxes and rotation only.
pub fn PDFKING_Page_LoadUnparsed(doc: FPDF_DOCUMENT, index: i32) -> Result<FPDF_PAGE, String> {
    let page = unsafe { raw::PDFKING_Page_LoadUnparsed(doc, index as c_int) };
    if page.is_null() {
        last_error("load page")
    } else {
        Ok(page)
    }
}

pub fn FPDF_ClosePage(page: FPDF_PAGE) {
    unsafe { raw::FPDF_ClosePage(page) }
}

pub fn FPDFPage_New(
    doc: FPDF_DOCUMENT,
    index: i32,
    width: f64,
    height: f64,
) -> Result<FPDF_PAGE, String> {
    let page =
        unsafe { raw::FPDFPage_New(doc, index as c_int, width as c_double, height as c_double) };
    if page.is_null() {
        last_error("create page")
    } else {
        Ok(page)
    }
}

pub fn FPDF_GetPageHeightF(page: FPDF_PAGE) -> f32 {
    unsafe { raw::FPDF_GetPageHeightF(page) as f32 }
}

pub fn FPDFPage_GetMediaBox(page: FPDF_PAGE) -> Option<(f32, f32, f32, f32)> {
    let (mut l, mut b, mut r, mut t) = (0f32, 0f32, 0f32, 0f32);
    if unsafe { raw::FPDFPage_GetMediaBox(page, &mut l, &mut b, &mut r, &mut t) } != 0 {
        Some((l, b, r, t))
    } else {
        None
    }
}

pub fn FPDFPage_GetTrimBox(page: FPDF_PAGE) -> Option<(f32, f32, f32, f32)> {
    let (mut l, mut b, mut r, mut t) = (0f32, 0f32, 0f32, 0f32);
    if unsafe { raw::FPDFPage_GetTrimBox(page, &mut l, &mut b, &mut r, &mut t) } != 0 {
        Some((l, b, r, t))
    } else {
        None
    }
}

/// The box pdfium shows the page through: its CropBox cut to its MediaBox,
/// or the MediaBox when it has none. A page made an XObject starts there.
/// pdfium only refuses a null page, which a loaded one never is.
pub fn FPDF_GetPageBoundingBox(page: FPDF_PAGE) -> (f32, f32, f32, f32) {
    let mut rect = FS_RECTF { left: 0.0, top: 0.0, right: 0.0, bottom: 0.0 };
    unsafe { raw::FPDF_GetPageBoundingBox(page, &mut rect) };
    (rect.left, rect.bottom, rect.right, rect.top)
}

pub fn FPDFPage_SetMediaBox(page: FPDF_PAGE, left: f32, bottom: f32, right: f32, top: f32) {
    unsafe { raw::FPDFPage_SetMediaBox(page, left, bottom, right, top) }
}

pub fn FPDFPage_SetTrimBox(page: FPDF_PAGE, left: f32, bottom: f32, right: f32, top: f32) {
    unsafe { raw::FPDFPage_SetTrimBox(page, left, bottom, right, top) }
}

pub fn FPDFPage_GenerateContent(page: FPDF_PAGE) -> Result<(), String> {
    if unsafe { raw::FPDFPage_GenerateContent(page) } == 0 {
        last_error("generate content")
    } else {
        Ok(())
    }
}

pub fn FPDFPage_InsertObject(page: FPDF_PAGE, obj: FPDF_PAGEOBJECT) {
    // Returns FPDF_BOOL since pdfium main; failure surfaces via GenerateContent.
    unsafe { raw::FPDFPage_InsertObject(page, obj) };
}

/// The page's /Rotate in quarter turns, 0 to 3.
pub fn FPDFPage_GetRotation(page: FPDF_PAGE) -> i32 {
    unsafe { raw::FPDFPage_GetRotation(page) }
}

/// Renders through `matrix`, applied after the page's display matrix (the
/// page as drawn, y-down, in points), clipped to `clipping` in bitmap pixels.
pub fn FPDF_RenderPageBitmapWithMatrix(
    bitmap: FPDF_BITMAP,
    page: FPDF_PAGE,
    matrix: &FS_MATRIX,
    clipping: &FS_RECTF,
    flags: i32,
) {
    unsafe { raw::FPDF_RenderPageBitmapWithMatrix(bitmap, page, matrix, clipping, flags) }
}

// --- Image objects ---

pub fn FPDFPageObj_NewImageObj(doc: FPDF_DOCUMENT) -> Result<FPDF_PAGEOBJECT, String> {
    let obj = unsafe { raw::FPDFPageObj_NewImageObj(doc) };
    if obj.is_null() {
        last_error("create image object")
    } else {
        Ok(obj)
    }
}

pub fn FPDFImageObj_LoadJpegFileInline(
    page: &mut FPDF_PAGE,
    image_obj: FPDF_PAGEOBJECT,
    file_access: &mut FPDF_FILEACCESS,
) -> Result<(), String> {
    if unsafe {
        raw::FPDFImageObj_LoadJpegFileInline(page as *mut FPDF_PAGE, 1, image_obj, file_access)
    } == 0
    {
        last_error("load jpeg")
    } else {
        Ok(())
    }
}

pub fn FPDFImageObj_GetImagePixelSize(image_obj: FPDF_PAGEOBJECT) -> Result<(u32, u32), String> {
    let (mut w, mut h) = (0u32, 0u32);
    if unsafe {
        raw::FPDFImageObj_GetImagePixelSize(
            image_obj,
            &mut w as *mut u32 as *mut c_uint,
            &mut h as *mut u32 as *mut c_uint,
        )
    } == 0
    {
        last_error("get image pixel size")
    } else {
        Ok((w, h))
    }
}

pub fn FPDFImageObj_SetBitmap(
    page: &mut FPDF_PAGE,
    image_obj: FPDF_PAGEOBJECT,
    bitmap: FPDF_BITMAP,
) -> Result<(), String> {
    if unsafe { raw::FPDFImageObj_SetBitmap(page as *mut FPDF_PAGE, 1, image_obj, bitmap) } == 0 {
        last_error("set bitmap")
    } else {
        Ok(())
    }
}

// --- Bitmap ---

pub fn FPDFBitmap_CreateEx(
    width: i32,
    height: i32,
    format: i32,
    buffer: *mut c_void,
    stride: i32,
) -> Result<FPDF_BITMAP, String> {
    let bm = unsafe { raw::FPDFBitmap_CreateEx(width, height, format, buffer, stride) };
    if bm.is_null() {
        last_error("create bitmap")
    } else {
        Ok(bm)
    }
}

pub fn FPDFBitmap_Destroy(bitmap: FPDF_BITMAP) {
    unsafe { raw::FPDFBitmap_Destroy(bitmap) }
}

// --- Transform ---

pub fn FPDFPageObj_Transform(obj: FPDF_PAGEOBJECT, a: f64, b: f64, c: f64, d: f64, e: f64, f: f64) {
    unsafe {
        raw::FPDFPageObj_Transform(
            obj,
            a as c_double,
            b as c_double,
            c as c_double,
            d as c_double,
            e as c_double,
            f as c_double,
        )
    }
}

pub fn FPDFPageObj_GetBounds(obj: FPDF_PAGEOBJECT) -> (f32, f32, f32, f32) {
    let (mut l, mut b, mut r, mut t) = (0f32, 0f32, 0f32, 0f32);
    unsafe { raw::FPDFPageObj_GetBounds(obj, &mut l, &mut b, &mut r, &mut t) };
    (l, b, r, t)
}

pub fn FPDFPageObj_SetStrokeWidth(obj: FPDF_PAGEOBJECT, width: f32) {
    unsafe { raw::FPDFPageObj_SetStrokeWidth(obj, width as c_float) };
}

// --- Path / Rect ---

pub fn FPDFPageObj_CreateNewRect(
    x: f32,
    y: f32,
    w: f32,
    h: f32,
) -> Result<FPDF_PAGEOBJECT, String> {
    let obj = unsafe { raw::FPDFPageObj_CreateNewRect(x, y, w, h) };
    if obj.is_null() {
        Err(String::from("create rect failed"))
    } else {
        Ok(obj)
    }
}

pub fn FPDFPageObj_CreateNewPath(x: f32, y: f32) -> Result<FPDF_PAGEOBJECT, String> {
    let obj = unsafe { raw::FPDFPageObj_CreateNewPath(x, y) };
    if obj.is_null() {
        Err(String::from("create path failed"))
    } else {
        Ok(obj)
    }
}

pub fn FPDFPath_BezierTo(path: FPDF_PAGEOBJECT, x1: f32, y1: f32, x2: f32, y2: f32, x3: f32, y3: f32) {
    unsafe { raw::FPDFPath_BezierTo(path, x1, y1, x2, y2, x3, y3) };
}

pub fn FPDFPath_Close(path: FPDF_PAGEOBJECT) {
    unsafe { raw::FPDFPath_Close(path) };
}

pub fn FPDFPath_SetDrawMode(path: FPDF_PAGEOBJECT, fillmode: i32, stroke: i32) {
    unsafe { raw::FPDFPath_SetDrawMode(path, fillmode as c_int, stroke as c_int) };
}

// --- Text ---

pub fn FPDFPageObj_NewTextObj(
    doc: FPDF_DOCUMENT,
    font: *const c_char,
    font_size: f32,
) -> Result<FPDF_PAGEOBJECT, String> {
    let obj = unsafe { raw::FPDFPageObj_NewTextObj(doc, font, font_size as c_float) };
    if obj.is_null() {
        last_error("create text object")
    } else {
        Ok(obj)
    }
}

pub fn FPDFText_SetText(text_obj: FPDF_PAGEOBJECT, text: *const u16) {
    unsafe { raw::FPDFText_SetText(text_obj, text) };
}

// --- XObject ---

pub fn FPDF_NewXObjectFromPage(
    dest: FPDF_DOCUMENT,
    src: FPDF_DOCUMENT,
    page_index: i32,
) -> Result<FPDF_XOBJECT, String> {
    let xobj = unsafe { raw::FPDF_NewXObjectFromPage(dest, src, page_index as c_int) };
    if xobj.is_null() {
        last_error("create xobject")
    } else {
        Ok(xobj)
    }
}

pub fn FPDF_CloseXObject(xobject: FPDF_XOBJECT) {
    unsafe { raw::FPDF_CloseXObject(xobject) }
}

pub fn FPDF_NewFormObjectFromXObject(xobject: FPDF_XOBJECT) -> Result<FPDF_PAGEOBJECT, String> {
    let obj = unsafe { raw::FPDF_NewFormObjectFromXObject(xobject) };
    if obj.is_null() {
        last_error("create form object")
    } else {
        Ok(obj)
    }
}

// --- Availability ---

pub fn FPDFAvail_Create(
    file_avail: &mut FX_FILEAVAIL,
    file_access: &mut FPDF_FILEACCESS,
) -> Result<FPDF_AVAIL, String> {
    let avail = unsafe { raw::FPDFAvail_Create(file_avail, file_access) };
    if avail.is_null() {
        last_error("create avail")
    } else {
        Ok(avail)
    }
}

pub fn FPDFAvail_Destroy(avail: FPDF_AVAIL) {
    unsafe { raw::FPDFAvail_Destroy(avail) }
}

pub fn FPDFAvail_IsDocAvail(avail: FPDF_AVAIL, hints: &mut FX_DOWNLOADHINTS) -> i32 {
    unsafe { raw::FPDFAvail_IsDocAvail(avail, hints) as i32 }
}

pub fn FPDFAvail_GetDocument(avail: FPDF_AVAIL) -> Result<FPDF_DOCUMENT, String> {
    let doc = unsafe { raw::FPDFAvail_GetDocument(avail, core::ptr::null()) };
    if doc.is_null() {
        last_error("get document from avail")
    } else {
        Ok(doc)
    }
}

pub fn FPDFAvail_IsPageAvail(
    avail: FPDF_AVAIL,
    page_index: i32,
    hints: &mut FX_DOWNLOADHINTS,
) -> i32 {
    unsafe { raw::FPDFAvail_IsPageAvail(avail, page_index as c_int, hints) as i32 }
}

// --- PDFKING (custom C) ---

/// CMYK pixels (255 the full ink) in RGB, as pdfium rasters DeviceCMYK
pub fn PDFKING_CmykToRgb(cmyk: &[u8]) -> Vec<u8> {
    let pixels = cmyk.len() / 4;
    let mut rgb = alloc::vec![0u8; pixels * 3];
    unsafe { raw::PDFKING_CmykToRgb(cmyk.as_ptr(), rgb.as_mut_ptr(), pixels as c_uint) }
    rgb
}

pub fn PDFKING_PageObj_SetFillRGBA(obj: FPDF_PAGEOBJECT, r: f32, g: f32, b: f32, a: f32) {
    unsafe { raw::PDFKING_PageObj_SetFillRGBA(obj, r, g, b, a) }
}

pub fn PDFKING_PageObj_SetStrokeRGBA(obj: FPDF_PAGEOBJECT, r: f32, g: f32, b: f32, a: f32) {
    unsafe { raw::PDFKING_PageObj_SetStrokeRGBA(obj, r, g, b, a) }
}

pub fn PDFKING_PageObj_SetFillCMYKA(obj: FPDF_PAGEOBJECT, c: f32, m: f32, y: f32, k: f32, a: f32) {
    unsafe { raw::PDFKING_PageObj_SetFillCMYKA(obj, c, m, y, k, a) }
}

pub fn PDFKING_PageObj_SetStrokeCMYKA(
    obj: FPDF_PAGEOBJECT,
    c: f32,
    m: f32,
    y: f32,
    k: f32,
    a: f32,
) {
    unsafe { raw::PDFKING_PageObj_SetStrokeCMYKA(obj, c, m, y, k, a) }
}

pub fn PDFKING_PageObj_SetClipRect(
    obj: FPDF_PAGEOBJECT,
    left: f32,
    bottom: f32,
    right: f32,
    top: f32,
) {
    unsafe { raw::PDFKING_PageObj_SetClipRect(obj, left, bottom, right, top) };
}

pub fn PDFKING_XObject_CopyGroup(page: FPDF_PAGE, xobject: FPDF_XOBJECT) {
    unsafe { raw::PDFKING_XObject_CopyGroup(page, xobject) };
}

// --- PDFKING Document internals (for page export) ---

pub fn PDFKING_Doc_FromFPDF(doc: FPDF_DOCUMENT) -> Result<*mut c_void, String> {
    let p = unsafe { raw::PDFKING_Doc_FromFPDF(doc) };
    if p.is_null() {
        Err(String::from("PDFKING_Doc_FromFPDF: null"))
    } else {
        Ok(p)
    }
}

pub fn PDFKING_Doc_SetLastObjNum(doc: *mut c_void, num: u32) {
    unsafe { raw::PDFKING_Doc_SetLastObjNum(doc, num as c_uint) }
}

pub fn PDFKING_Doc_GetLastObjNum(doc: *mut c_void) -> u32 {
    unsafe { raw::PDFKING_Doc_GetLastObjNum(doc) as u32 }
}

pub fn PDFKING_Doc_ExportPage(
    dest: *mut c_void,
    src: *mut c_void,
    page_index: u32,
    insert_index: i32,
) -> Result<(), String> {
    if unsafe {
        raw::PDFKING_Doc_ExportPage(dest, src, page_index as c_uint, insert_index as c_int)
    } == 0
    {
        last_error("export page")
    } else {
        Ok(())
    }
}

pub fn PDFKING_Doc_GetObject(doc: *mut c_void, objnum: u32) -> *mut c_void {
    unsafe { raw::PDFKING_Doc_GetObject(doc, objnum as c_uint) }
}

pub fn PDFKING_Object_PageTag(obj: *mut c_void) -> i32 {
    unsafe { raw::PDFKING_Object_PageTag(obj) as i32 }
}

pub fn PDFKING_Page_SetParentRef(obj: *mut c_void, doc: *mut c_void, parent_objnum: u32) {
    unsafe { raw::PDFKING_Page_SetParentRef(obj, doc, parent_objnum as c_uint) }
}

pub fn PDFKING_Object_Serialize(obj: *mut c_void, out: &mut Vec<u8>) -> Result<(), String> {
    if unsafe { raw::PDFKING_Object_Serialize(obj, push_bytes, out as *mut _ as *mut c_void) } == 0 {
        return Err("PDFKING_Object_Serialize failed".into());
    }
    Ok(())
}

// --- PDFKING catalog: layers and the shared output intent / XMP ---

/// Tags every layer of `doc` with its state, for the copies made of it
pub fn PDFKING_Doc_TagLayers(doc: *mut c_void) {
    unsafe { raw::PDFKING_Doc_TagLayers(doc) }
}

/// `None` when `obj` is no OCG, else its tagged state (see `LAYER_*`)
pub fn PDFKING_Object_LayerFlags(obj: *mut c_void) -> Option<i32> {
    let flags = unsafe { raw::PDFKING_Object_LayerFlags(obj) };
    (flags >= 0).then_some(flags as i32)
}

/// The OCG's /Name
pub fn PDFKING_Object_LayerName(obj: *mut c_void) -> String {
    String::from_utf8_lossy(&gathered(|cb, ctx| unsafe {
        raw::PDFKING_Object_LayerName(obj, cb, ctx);
    }))
    .into_owned()
}

/// References to `from[i]` among objects `first..=last` made to `to[i]`
pub fn PDFKING_Doc_RemapRefs(doc: *mut c_void, first: u32, last: u32, from: &[u32], to: &[u32]) {
    unsafe {
        raw::PDFKING_Doc_RemapRefs(doc, first, last, from.as_ptr(), to.as_ptr(), from.len() as c_uint)
    }
}

pub const LAYER_OFF: i32 = 1;
pub const LAYER_LOCKED: i32 = 2;
pub const LAYER_HIDDEN: i32 = 4;
pub const LAYER_NOPRINT: i32 = 8;

/// The OCG's /Usage set as `flags` say: left out of print or not
pub fn PDFKING_Object_SetLayerPrint(obj: *mut c_void, flags: i32) {
    unsafe { raw::PDFKING_Object_SetLayerPrint(obj, flags as c_int) }
}

pub fn PDFKING_Doc_SetLayers(doc: *mut c_void, objnums: &[u32], flags: &[i32]) {
    unsafe {
        raw::PDFKING_Doc_SetLayers(doc, objnums.as_ptr(), flags.as_ptr(), objnums.len() as c_uint)
    }
}

/// A new OCG named `name`, tagged with `flags` (see `LAYER_*`)
pub fn PDFKING_Doc_NewLayer(doc: *mut c_void, name: &str, flags: i32) -> Result<u32, String> {
    let name = alloc::ffi::CString::new(name).map_err(|_| String::from("invalid layer name"))?;
    Ok(unsafe { raw::PDFKING_Doc_NewLayer(doc, name.as_ptr(), flags as c_int) as u32 })
}

pub fn PDFKING_PageObj_SetLayer(page: FPDF_PAGE, obj: FPDF_PAGEOBJECT, objnum: u32) {
    unsafe { raw::PDFKING_PageObj_SetLayer(page, obj, objnum as c_uint) }
}

/// A Separation for the spot ink `name`, `c m y k` its alternate: its object number
pub fn PDFKING_Doc_NewSeparation(doc: *mut c_void, name: &str, c: f32, m: f32, y: f32, k: f32) -> Result<u32, String> {
    let name = alloc::ffi::CString::new(name).map_err(|_| String::from("invalid spot name"))?;
    Ok(unsafe { raw::PDFKING_Doc_NewSeparation(doc, name.as_ptr(), c, m, y, k) as u32 })
}

pub fn PDFKING_PageObj_SetFillSeparation(page: FPDF_PAGE, obj: FPDF_PAGEOBJECT, objnum: u32, a: f32) {
    unsafe { raw::PDFKING_PageObj_SetFillSeparation(page, obj, objnum as c_uint, a) }
}

pub fn PDFKING_PageObj_SetStrokeSeparation(page: FPDF_PAGE, obj: FPDF_PAGEOBJECT, objnum: u32, a: f32) {
    unsafe { raw::PDFKING_PageObj_SetStrokeSeparation(page, obj, objnum as c_uint, a) }
}

/// `(output intents, metadata)` object numbers in `dest`, 0 for none
pub fn PDFKING_Doc_ExportShared(dest: *mut c_void, src: *mut c_void) -> (u32, u32) {
    let (mut intents, mut metadata): (c_uint, c_uint) = (0, 0);
    unsafe { raw::PDFKING_Doc_ExportShared(dest, src, &mut intents, &mut metadata) };
    (intents as u32, metadata as u32)
}

// --- PDFKING object primitives: a document's objects, one at a time ---
//
// An object is a pointer the document owns, null for none: every function
// takes null and answers nothing.

/// `size` bytes at `data`: none for a null pointer, as C hands an empty one
unsafe fn bytes<'a>(data: *const u8, size: u32) -> &'a [u8] {
    if data.is_null() { &[] } else { core::slice::from_raw_parts(data, size as usize) }
}

/// bytes handed to a callback, gathered
unsafe extern "C" fn push_bytes(data: *const u8, size: u32, ctx: *mut c_void) -> c_int {
    let buf = &mut *(ctx as *mut Vec<u8>);
    buf.extend_from_slice(bytes(data, size));
    size as c_int
}

fn gathered(call: impl FnOnce(unsafe extern "C" fn(*const u8, u32, *mut c_void) -> c_int, *mut c_void)) -> Vec<u8> {
    let mut buf = Vec::new();
    call(push_bytes, &mut buf as *mut _ as *mut c_void);
    buf
}

pub const KIND_BOOLEAN: i32 = 1;
pub const KIND_NUMBER: i32 = 2;
pub const KIND_STRING: i32 = 3;
pub const KIND_NAME: i32 = 4;
pub const KIND_ARRAY: i32 = 5;
pub const KIND_DICTIONARY: i32 = 6;
pub const KIND_STREAM: i32 = 7;
pub const KIND_NULL: i32 = 8;
pub const KIND_REFERENCE: i32 = 9;

/// What `obj` is (see `KIND_*`), 0 for none
pub fn PDFKING_Object_Kind(obj: *mut c_void) -> i32 {
    unsafe { raw::PDFKING_Object_Kind(obj) as i32 }
}

/// What a reference refers to, parsed if nothing has read it yet; any other
/// object itself
pub fn PDFKING_Object_Direct(obj: *mut c_void) -> *mut c_void {
    unsafe { raw::PDFKING_Object_Direct(obj) }
}

/// The number of the object a reference refers to, or of `obj` itself: 0
/// for a direct one
pub fn PDFKING_Object_Num(obj: *mut c_void) -> u32 {
    unsafe { raw::PDFKING_Object_Num(obj) as u32 }
}

pub fn PDFKING_Object_Number(obj: *mut c_void) -> f64 {
    unsafe { raw::PDFKING_Object_Number(obj) }
}

/// A name's or a string's bytes
pub fn PDFKING_Object_Bytes(obj: *mut c_void) -> Vec<u8> {
    gathered(|cb, ctx| unsafe {
        raw::PDFKING_Object_Bytes(obj, cb, ctx);
    })
}

/// A text string
pub fn PDFKING_Object_Text(obj: *mut c_void) -> String {
    String::from_utf8_lossy(&gathered(|cb, ctx| unsafe {
        raw::PDFKING_Object_Text(obj, cb, ctx);
    }))
    .into_owned()
}

/// The entry of a dictionary (a stream's, or the one a reference refers
/// to) as it holds it: a reference stays one
pub fn PDFKING_Dict_Get(obj: *mut c_void, key: &[u8]) -> *mut c_void {
    unsafe { raw::PDFKING_Dict_Get(obj, key.as_ptr(), key.len() as c_uint) }
}

/// Every entry, in the dictionary's order
pub fn PDFKING_Dict_Each(obj: *mut c_void, mut each: impl FnMut(&[u8], *mut c_void)) {
    unsafe extern "C" fn entry<F: FnMut(&[u8], *mut c_void)>(
        key: *const u8,
        size: u32,
        value: *mut c_void,
        ctx: *mut c_void,
    ) -> c_int {
        (*(ctx as *mut F))(bytes(key, size), value);
        1
    }
    fn call<F: FnMut(&[u8], *mut c_void)>(obj: *mut c_void, each: &mut F) {
        unsafe { raw::PDFKING_Dict_Each(obj, entry::<F>, each as *mut F as *mut c_void) }
    }
    call(obj, &mut each)
}

pub fn PDFKING_Array_Size(obj: *mut c_void) -> u32 {
    unsafe { raw::PDFKING_Array_Size(obj) as u32 }
}

pub fn PDFKING_Array_Get(obj: *mut c_void, index: u32) -> *mut c_void {
    unsafe { raw::PDFKING_Array_Get(obj, index as c_uint) }
}

/// A stream's data, decoded (`filtered`) or as the file holds it
pub fn PDFKING_Stream_Data(obj: *mut c_void, filtered: bool) -> Vec<u8> {
    gathered(|cb, ctx| unsafe {
        raw::PDFKING_Stream_Data(obj, filtered as c_int, cb, ctx);
    })
}

/// The objects `obj` refers to, as a page export follows them: not through
/// /Prev, /First, nor a /Parent other than a popup's
pub fn PDFKING_Object_Refs(obj: *mut c_void, refs: &mut Vec<u32>) {
    unsafe extern "C" fn push(objnum: c_uint, ctx: *mut c_void) {
        (*(ctx as *mut Vec<u32>)).push(objnum as u32)
    }
    unsafe { raw::PDFKING_Object_Refs(obj, push, refs as *mut _ as *mut c_void) }
}

/// The dictionary of page `page_index`
pub fn PDFKING_Doc_PageDict(doc: *mut c_void, page_index: u32) -> *mut c_void {
    unsafe { raw::PDFKING_Doc_PageDict(doc, page_index as c_uint) }
}

/// The catalog
pub fn PDFKING_Doc_Root(doc: *mut c_void) -> *mut c_void {
    unsafe { raw::PDFKING_Doc_Root(doc) }
}

/// Whether what is on the layer `oc` (an OCG or a membership dictionary)
/// prints
pub fn PDFKING_Doc_LayerPrints(doc: *mut c_void, oc: *mut c_void) -> bool {
    unsafe { raw::PDFKING_Doc_LayerPrints(doc, oc) != 0 }
}

/// A new dictionary, the caller's until [`PDFKING_Object_Release`]
pub fn PDFKING_Dict_New() -> *mut c_void {
    unsafe { raw::PDFKING_Dict_New() }
}

pub fn PDFKING_Object_Release(obj: *mut c_void) {
    unsafe { raw::PDFKING_Object_Release(obj) }
}

/// `value` itself set in the dictionary, which holds it from then on too
pub fn PDFKING_Dict_Set(obj: *mut c_void, key: &[u8], value: *mut c_void) {
    unsafe { raw::PDFKING_Dict_Set(obj, key.as_ptr(), key.len() as c_uint, value) }
}

/// A copy of `value` set in the dictionary
pub fn PDFKING_Dict_SetClone(obj: *mut c_void, key: &[u8], value: *mut c_void) {
    unsafe { raw::PDFKING_Dict_SetClone(obj, key.as_ptr(), key.len() as c_uint, value) }
}

/// The stream's data, not encoded
pub fn PDFKING_Stream_SetData(obj: *mut c_void, data: &[u8]) {
    unsafe { raw::PDFKING_Stream_SetData(obj, data.as_ptr(), data.len() as c_uint) }
}

/// Raw deflate, a segment at a time: each ends byte aligned on a sync
/// flush and is not the last block, so segments join into one stream
pub struct Deflater(*mut c_void);

impl Deflater {
    /// zlib's `level`: 1 fastest, 9 smallest
    pub fn new(level: i32) -> Self {
        Self(unsafe { raw::PDFKING_Deflater_New(level as c_int) })
    }

    /// `data` as one segment appended to `out`; its adler32
    pub fn segment(&self, data: &[u8], out: &mut Vec<u8>) -> u32 {
        unsafe {
            raw::PDFKING_Deflater_Segment(
                self.0,
                data.as_ptr(),
                data.len() as c_uint,
                push_bytes,
                out as *mut _ as *mut c_void,
            ) as u32
        }
    }
}

impl Drop for Deflater {
    fn drop(&mut self) {
        unsafe { raw::PDFKING_Deflater_Free(self.0) }
    }
}

// --- lcms2 (linked from libpdfium.a) ---

pub fn cmsCreate_sRGBProfile() -> Result<*mut c_void, String> {
    let p = unsafe { raw::cmsCreate_sRGBProfile() };
    if p.is_null() {
        Err(String::from("failed to create sRGB profile"))
    } else {
        Ok(p)
    }
}

pub fn cmsOpenProfileFromMem(data: &[u8]) -> Result<*mut c_void, String> {
    let p =
        unsafe { raw::cmsOpenProfileFromMem(data.as_ptr() as *const c_void, data.len() as c_uint) };
    if p.is_null() {
        Err(String::from("failed to open ICC profile"))
    } else {
        Ok(p)
    }
}

pub fn cmsCreateProofingTransform(
    input: *mut c_void,
    input_fmt: u32,
    output: *mut c_void,
    output_fmt: u32,
    proofing: *mut c_void,
    intent: u32,
    proofing_intent: u32,
    flags: u32,
) -> Result<*mut c_void, String> {
    let p = unsafe {
        raw::cmsCreateProofingTransform(
            input,
            input_fmt,
            output,
            output_fmt,
            proofing,
            intent,
            proofing_intent,
            flags,
        )
    };
    if p.is_null() {
        Err(String::from("failed to create proofing transform"))
    } else {
        Ok(p)
    }
}

pub fn cmsDoTransform(
    transform: *mut c_void,
    input: *const c_void,
    output: *mut c_void,
    size: u32,
) {
    unsafe { raw::cmsDoTransform(transform, input, output, size as c_uint) }
}

pub fn cmsDeleteTransform(transform: *mut c_void) {
    unsafe { raw::cmsDeleteTransform(transform) }
}

pub fn cmsCloseProfile(profile: *mut c_void) {
    unsafe { raw::cmsCloseProfile(profile) };
}
