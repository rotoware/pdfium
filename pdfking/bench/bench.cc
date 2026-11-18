// pdfking: the plates of a PDF's first page rendered in one process, as
// switch separate wants them: 8-bit gray, a band of rows at a time, the
// document read once. Times each plate; --dump writes the plates raw.
// usage: bench <pdf> <dpi> <rows> <plate,plate,...> [--bgra] [--dump=<prefix>]

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "public/fpdfview.h"

static double Now() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: bench <pdf> <dpi> <rows> <plates> [--bgra] [--dump=prefix]\n");
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
  std::string dump;
  for (int i = 5; i < argc; ++i) {
    if (!strcmp(argv[i], "--bgra")) {
      bgra = true;
    } else if (!strncmp(argv[i], "--dump=", 7)) {
      dump = argv[i] + 7;
    }
  }

  double start = Now();
  FPDF_LIBRARY_CONFIG config = {};
  config.version = 2;
  FPDF_InitLibraryWithConfig(&config);
  FPDF_DOCUMENT doc = FPDF_LoadDocument(path, nullptr);
  if (!doc) {
    fprintf(stderr, "cannot open %s: %lu\n", path, FPDF_GetLastError());
    return 1;
  }
  int flags = FPDF_PRINTING | FPDF_NO_NATIVETEXT | FPDF_RENDER_NO_SMOOTHTEXT |
              FPDF_RENDER_NO_SMOOTHIMAGE | FPDF_RENDER_NO_SMOOTHPATH;
  fprintf(stderr, "open %.3fs\n", Now() - start);

  for (const std::string& plate : plates) {
    double began = Now();
    PDFKING_SetPlate(doc, plate.c_str());
    // the page loaded again a plate: its image cache holds the last plate's
    FPDF_PAGE page = FPDF_LoadPage(doc, 0);
    // the page's size rounded, as gs sizes it; drawn at dpi / 72 exactly
    int width = static_cast<int>(FPDF_GetPageWidthF(page) * dpi / 72 + 0.5);
    int height = static_cast<int>(FPDF_GetPageHeightF(page) * dpi / 72 + 0.5);
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
      FS_MATRIX matrix = {static_cast<float>(dpi / 72), 0, 0,
                          static_cast<float>(dpi / 72), 0,
                          static_cast<float>(-top)};
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
