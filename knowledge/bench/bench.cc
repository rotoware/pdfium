// pdfking: the plates of a PDF's first page rendered in one process, as
// switch separate wants them: 8-bit gray, a band of rows at a time, the
// document read once. Times each plate; --dump writes the plates raw.
// usage: bench <pdf> <dpi> <rows> <plate,plate,...> [--bgra] [--dump=<prefix>]
//        [--page=n] (0-based, else the first)
//        [--gsfonts=<dir>] (gs's share/ghostscript: fonts not embedded as
//        gs loads them, PDFKING_SetGsFontDir)
//        [--profiles=<dir>] (gs's iccprofiles: default_rgb.icc and
//        default_cmyk.icc, PDFKING_SetBlendProfiles: RGB page groups as gs)
//        [--crop=x,y,w,h] (pixels from the top left: that window alone)
//        [--ink] (all plates in one render a band, PDFKING_RenderInks)
//        <plates> "auto": the process plates and the page's spots
//        (PDFKING_GetPageInks), printed to stdout one a line

#include <chrono>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "public/fpdf_pdfking.h"

static double Now() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The plates in one render a band: PDFKING_RenderInks
static int Inks(FPDF_DOCUMENT doc,
                int page_index,
                const std::vector<std::string>& plates,
                double dpi,
                int rows,
                const std::string& dump,
                const int crop[4],
                int flags,
                double start) {
  double began = Now();
  FPDF_PAGE page = FPDF_LoadPage(doc, page_index);
  // pixels a unit of the page: its /UserUnit points
  const double scale = dpi / 72 * PDFKING_GetPageUserUnit(page);
  int width = static_cast<int>(FPDF_GetPageWidthF(page) * scale + 0.5);
  int height = static_cast<int>(FPDF_GetPageHeightF(page) * scale + 0.5);
  float lift = height - FPDF_GetPageHeightF(page) * static_cast<float>(scale);
  if (crop[2]) {
    width = crop[2];
    height = crop[3];
  }
  std::vector<FPDF_BITMAP> bands;
  std::vector<FPDF_BYTESTRING> names;
  PDFKING_INKS inks = nullptr;
  std::vector<FILE*> outs;
  for (const std::string& plate : plates) {
    bands.push_back(
        FPDFBitmap_CreateEx(width, rows, FPDFBitmap_Gray, nullptr, 0));
    names.push_back(plate.c_str());
    outs.push_back(nullptr);
    if (!dump.empty()) {
      outs.back() = fopen((dump + "-" + plate + ".pgm").c_str(), "wb");
      fprintf(outs.back(), "P5\n%d %d\n255\n", width, height);
    }
  }
  inks = PDFKING_LoadInks(page, names.data(), static_cast<int>(names.size()));
  int failed = 0;
  for (int top = 0; top < height; top += rows) {
    for (FPDF_BITMAP band : bands) {
      FPDFBitmap_FillRect(band, 0, 0, width, rows, 0xFFFFFFFF);
    }
    FS_MATRIX matrix = {static_cast<float>(scale),
                        0,
                        0,
                        static_cast<float>(scale),
                        static_cast<float>(-crop[0]),
                        static_cast<float>(-crop[1] - top) + lift};
    FS_RECTF clip = {0, 0, static_cast<float>(width), static_cast<float>(rows)};
    if (!PDFKING_RenderInks(inks, page, bands.data(), &matrix, &clip, flags)) {
      ++failed;
    }
    int n = std::min(rows, height - top);
    for (size_t i = 0; i < bands.size(); ++i) {
      if (!outs[i]) {
        continue;
      }
      const uint8_t* buffer =
          static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bands[i]));
      int stride = FPDFBitmap_GetStride(bands[i]);
      for (int y = 0; y < n; ++y) {
        fwrite(buffer + y * stride, 1, width, outs[i]);
      }
    }
  }
  for (size_t i = 0; i < bands.size(); ++i) {
    if (outs[i]) {
      fclose(outs[i]);
    }
    FPDFBitmap_Destroy(bands[i]);
  }
  PDFKING_CloseInks(inks);
  FPDF_ClosePage(page);
  fprintf(stderr, "inks %dx%d %.3fs%s\n", width, height, Now() - began,
          failed ? " FAILED" : "");
  FPDF_CloseDocument(doc);
  FPDF_DestroyLibrary();
  fprintf(stderr, "total %.3fs\n", Now() - start);
  return failed ? 2 : 0;
}

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(
        stderr,
        "usage: bench <pdf> <dpi> <rows> <plates> [--bgra] [--dump=prefix]\n");
    return 1;
  }
  const char* path = argv[1];
  double dpi = atof(argv[2]);
  int rows = atoi(argv[3]);
  std::vector<std::string> plates;
  for (std::string rest = argv[4]; !rest.empty();) {
    size_t comma = rest.find(',');
    plates.push_back(rest.substr(0, comma));
    rest = comma == std::string::npos ? "" : rest.substr(comma + 1);
  }
  bool bgra = false;
  bool ink = false;
  std::string dump;
  int crop[4] = {0, 0, 0, 0};
  int page_index = 0;
  std::string profiles;
  std::string gsfonts;
  for (int i = 5; i < argc; ++i) {
    if (!strcmp(argv[i], "--bgra")) {
      bgra = true;
    } else if (!strcmp(argv[i], "--ink")) {
      ink = true;
    } else if (!strncmp(argv[i], "--dump=", 7)) {
      dump = argv[i] + 7;
    } else if (!strncmp(argv[i], "--gsfonts=", 10)) {
      gsfonts = argv[i] + 10;
    } else if (!strncmp(argv[i], "--profiles=", 11)) {
      profiles = argv[i] + 11;
    } else if (!strncmp(argv[i], "--page=", 7)) {
      page_index = atoi(argv[i] + 7);
    } else if (!strncmp(argv[i], "--crop=", 7)) {
      sscanf(argv[i] + 7, "%d,%d,%d,%d", &crop[0], &crop[1], &crop[2],
             &crop[3]);
    }
  }

  double start = Now();
  FPDF_LIBRARY_CONFIG config = {};
  // 6: /BrotliDecode streams read (pdfium_test reads them too)
  config.version = 6;
  config.m_BrotliEnabled = true;
  FPDF_InitLibraryWithConfig(&config);
  if (!gsfonts.empty()) {
    PDFKING_SetGsFontDir(gsfonts.c_str());
  }
  FPDF_DOCUMENT doc = FPDF_LoadDocument(path, nullptr);
  if (!doc) {
    fprintf(stderr, "cannot open %s: %lu\n", path, FPDF_GetLastError());
    return 1;
  }
  if (!profiles.empty()) {
    auto read = [&](const char* name) {
      std::ifstream in(profiles + "/" + name, std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(in), {});
    };
    std::string rgb = read("default_rgb.icc");
    std::string cmyk = read("default_cmyk.icc");
    std::string gray = read("default_gray.icc");
    PDFKING_SetBlendProfiles(
        doc, reinterpret_cast<const unsigned char*>(rgb.data()), rgb.size(),
        reinterpret_cast<const unsigned char*>(cmyk.data()), cmyk.size(),
        reinterpret_cast<const unsigned char*>(gray.data()), gray.size());
  }
  int flags = FPDF_PRINTING | FPDF_ANNOT | FPDF_NO_NATIVETEXT |
              FPDF_RENDER_NO_SMOOTHTEXT | FPDF_RENDER_NO_SMOOTHIMAGE |
              FPDF_RENDER_NO_SMOOTHPATH;
  fprintf(stderr, "open %.3fs\n", Now() - start);

  if (page_index >= FPDF_GetPageCount(doc)) {
    fprintf(stderr, "no page %d\n", page_index);
    return 1;
  }
  if (plates.size() == 1 && plates[0] == "auto") {
    FPDF_PAGE page = FPDF_LoadPage(doc, page_index);
    plates = {"Cyan", "Magenta", "Yellow", "Black"};
    std::string names(PDFKING_GetPageInks(page, nullptr, 0), '\0');
    PDFKING_GetPageInks(page, names.data(), names.size());
    for (size_t at = 0; at < names.size();) {
      plates.emplace_back(names.c_str() + at);
      at += plates.back().size() + 1;
    }
    FPDF_ClosePage(page);
    for (const std::string& plate : plates) {
      printf("%s\n", plate.c_str());
    }
  }
  if (ink) {
    return Inks(doc, page_index, plates, dpi, rows, dump, crop, flags, start);
  }
  for (const std::string& plate : plates) {
    double began = Now();
    PDFKING_SetPlate(doc, plate.c_str());
    // the page loaded again a plate: its image cache holds the last plate's
    FPDF_PAGE page = FPDF_LoadPage(doc, page_index);
    // the page's size rounded, as gs sizes it (its /UserUnit points a
    // unit); drawn at that scale exactly
    const double scale = dpi / 72 * PDFKING_GetPageUserUnit(page);
    int width = static_cast<int>(FPDF_GetPageWidthF(page) * scale + 0.5);
    int height = static_cast<int>(FPDF_GetPageHeightF(page) * scale + 0.5);
    // pdfium puts the page's top on the bitmap's, gs (and switch's Install)
    // its bottom: a page some fraction of a pixel tall moves by it
    float lift = height - FPDF_GetPageHeightF(page) * static_cast<float>(scale);
    if (crop[2]) {
      width = crop[2];
      height = crop[3];
    }
    FPDF_BITMAP band = FPDFBitmap_CreateEx(
        width, rows, bgra ? FPDFBitmap_BGRx : FPDFBitmap_Gray, nullptr, 0);
    if (!band) {
      fprintf(stderr, "no bitmap %dx%d\n", width, rows);
      return 1;
    }
    FILE* out = nullptr;
    std::vector<uint8_t> gray;
    if (!dump.empty()) {
      out = fopen((dump + "-" + plate + ".pgm").c_str(), "wb");
      fprintf(out, "P5\n%d %d\n255\n", width, height);
      gray.resize(width);
    }
    unsigned long sum = 0;
    for (int top = 0; top < height; top += rows) {
      FPDFBitmap_FillRect(band, 0, 0, width, rows, 0xFFFFFFFF);
      FS_MATRIX matrix = {static_cast<float>(scale),
                          0,
                          0,
                          static_cast<float>(scale),
                          static_cast<float>(-crop[0]),
                          static_cast<float>(-crop[1] - top) + lift};
      FS_RECTF clip = {0, 0, static_cast<float>(width),
                       static_cast<float>(rows)};
      FPDF_RenderPageBitmapWithMatrix(band, page, &matrix, &clip, flags);
      const uint8_t* buffer =
          static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(band));
      int stride = FPDFBitmap_GetStride(band);
      int n = std::min(rows, height - top);
      // touch the band, as the screener reads it
      for (int y = 0; y < n; ++y) {
        sum += buffer[y * stride + (y % width) * (bgra ? 4 : 1)];
      }
      if (out) {
        for (int y = 0; y < n; ++y) {
          const uint8_t* row = buffer + y * stride;
          for (int x = 0; x < width; ++x) {
            gray[x] = bgra ? row[x * 4] : row[x];
          }
          fwrite(gray.data(), 1, width, out);
        }
      }
    }
    if (out) {
      fclose(out);
    }
    FPDFBitmap_Destroy(band);
    FPDF_ClosePage(page);
    fprintf(stderr, "%s %dx%d %.3fs (%lu)\n", plate.c_str(), width, height,
            Now() - began, sum);
  }
  FPDF_CloseDocument(doc);
  FPDF_DestroyLibrary();
  fprintf(stderr, "total %.3fs\n", Now() - start);
  return 0;
}
