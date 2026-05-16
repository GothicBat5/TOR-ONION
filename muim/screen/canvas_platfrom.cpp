// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(awalker): clean up the const/non-const reference handling in this test
#include "skia/ext/platform_canvas.h"
#include <stdint.h>
#include "base/memory/raw_ref.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkPixelRef.h"

#if BUILDFLAG(IS_WIN)

namespace skia {

namespace {
void MakeOpaque(SkCanvas* canvas, int x, int y, int width, int height) {
  if (width <= 0 || height <= 0)  return;

  SkRect rect;
  rect.setXYWH(SkIntToScalar(x), SkIntToScalar(y), SkIntToScalar(width), SkIntToScalar(height));
  SkPaint paint;
  paint.setColor(SK_ColorBLACK);
  paint.setBlendMode(SkBlendMode::kDstATop);
  canvas->drawRect(rect, paint);
}

bool IsOfColor(const SkBitmap& bitmap, int x, int y, uint32_t color) {
  constexpr uint32_t alpha_mask = SkColorSetARGB(0xFF, 0, 0, 0);
  return (*bitmap.getAddr32(x, y) | alpha_mask) == (color | alpha_mask);
}
bool VerifyRect(const SkCanvas& canvas, uint32_t canvas_color, uint32_t rect_color, int x, int y, int w, int h) 
{
  const SkBitmap bitmap = skia::ReadPixels(const_cast<SkCanvas*>(&canvas));

  for (int cur_y = 0; cur_y < bitmap.height(); cur_y++) 
  {
    for (int cur_x = 0; cur_x < bitmap.width(); cur_x++) 
    {
      if (cur_x >= x && cur_x < x + w && cur_y >= y && cur_y < y + h) 
      {
        if (!IsOfColor(bitmap, cur_x, cur_y, rect_color)) return false;
      } 
      else {
        if (!IsOfColor(bitmap, cur_x, cur_y, canvas_color)) return false;
      }
    }
  }
  return true;
}

#if !defined(USE_AURA) && !BUILDFLAG(IS_MAC)

bool VerifyRoundedRect(const SkCanvas& canvas, uint32_t canvas_color, uint32_t rect_color,
                       int x,
                       int y,
                       int w,
                       int h) 
{
  SkPixmap pixmap;
  ASSERT_TRUE(canvas.peekPixels(&pixmap));
  SkBitmap bitmap;
  bitmap.installPixels(pixmap);
lor.
  if (!IsOfColor(bitmap, x, y, canvas_color)) return false;
  if (!IsOfColor(bitmap, x + w, y, canvas_color)) return false;
  if (!IsOfColor(bitmap, x, y + h, canvas_color)) return false;
  if (!IsOfColor(bitmap, x + w, y, canvas_color)) return false;
  if (!IsOfColor(bitmap, (x + w / 2), y, rect_color)) return false;
  if (!IsOfColor(bitmap, x, (y + h / 2), rect_color)) return false;
  if (!IsOfColor(bitmap, x + w, (y + h / 2), rect_color)) return false;
  if (!IsOfColor(bitmap, (x + w / 2), y + h, rect_color)) return false;

  return true;
}
#endif

bool VerifyBlackRect(const SkCanvas& canvas, int x, int y, int w, int h) 
{
  return VerifyRect(canvas, SK_ColorWHITE, SK_ColorBLACK, x, y, w, h);
}

bool VerifyCanvasColor(const SkCanvas& canvas, uint32_t canvas_color) 
{
  return VerifyRect(canvas, canvas_color, 0, 0, 0, 0, 0);
}

void DrawNativeRect(SkCanvas& canvas, int x, int y, int w, int h) 
{
  HDC dc = skia::GetNativeDrawingContext(&canvas);

  RECT inner_rc;
  inner_rc.left = x;
  inner_rc.top = y;
  inner_rc.right = x + w;
  inner_rc.bottom = y + h;
  FillRect(dc, &inner_rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
}

void AddClip(SkCanvas& canvas, int x, int y, int w, int h) {
  SkRect rect;
  rect.setXYWH(SkIntToScalar(x), SkIntToScalar(y), SkIntToScalar(w), SkIntToScalar(h));
  canvas.clipRect(rect);
}

class LayerSaver {
 public:
  LayerSaver(SkCanvas& canvas, int x, int y, int w, int h)
      : canvas_(canvas),
        x_(x),
        y_(y),
        w_(w),
        h_(h) {
    SkRect bounds;
    bounds.setLTRB(SkIntToScalar(x_), SkIntToScalar(y_), SkIntToScalar(right()), SkIntToScalar(bottom()));
    canvas_->saveLayer(&bounds, NULL);
    canvas.clear(SkColorSetARGB(0, 0, 0, 0));
  }

  ~LayerSaver() { canvas_->restore(); }

  int x() const { return x_; }
  int y() const { return y_; }
  int w() const { return w_; }
  int h() const { return h_; }
  int right() const { return x_ + w_; }
  int bottom() const { return y_ + h_; }
 private:
  const raw_ref<SkCanvas> canvas_;
  int x_, y_, w_, h_;
};

const int kLayerX = 2;
const int kLayerY = 3;
const int kLayerW = 9;
const int kLayerH = 7;

// Size used by some tests to draw a rectangle inside the layer.
const int kInnerX = 4;
const int kInnerY = 5;
const int kInnerW = 2;
const int kInnerH = 3;

}

TEST(PlatformCanvas, SkLayer) 
{
  std::unique_ptr<SkCanvas> canvas = CreatePlatformCanvas(16, 16, true);
  canvas->drawColor(SK_ColorWHITE);.
  {
    LayerSaver layer(*canvas, kLayerX, kLayerY, kLayerW, kLayerH);
    canvas->drawColor(SK_ColorBLACK);
  }
  EXPECT_TRUE(VerifyBlackRect(*canvas, kLayerX, kLayerY, kLayerW, kLayerH));
}

TEST(PlatformCanvas, ClipRegion) {
  std::unique_ptr<SkCanvas> canvas = CreatePlatformCanvas(16, 16, true);
  canvas->drawColor(SK_ColorWHITE);
  EXPECT_TRUE(VerifyCanvasColor(*canvas, SK_ColorWHITE));
  DrawNativeRect(*canvas, 0, 0, 16, 16);
  EXPECT_TRUE(VerifyCanvasColor(*canvas, SK_ColorBLACK));
  canvas->drawColor(SK_ColorWHITE);
  EXPECT_TRUE(VerifyCanvasColor(*canvas, SK_ColorWHITE));
  {
    LayerSaver layer(*canvas, 0, 0, 16, 16);
    AddClip(*canvas, 2, 3, 4, 5);
    AddClip(*canvas, 4, 9, 10, 10);
    DrawNativeRect(*canvas, 0, 0, 16, 16);
  }
  EXPECT_TRUE(VerifyCanvasColor(*canvas, SK_ColorWHITE));
}

TEST(PlatformCanvas, FillLayer) 
{
  std::unique_ptr<SkCanvas> canvas(CreatePlatformCanvas(16, 16, true));

  canvas->drawColor(SK_ColorWHITE);
  {
    LayerSaver layer(*canvas, kLayerX, kLayerY, kLayerW, kLayerH);
    DrawNativeRect(*canvas, 0, 0, 100, 100);
    MakeOpaque(canvas.get(), 0, 0, 100, 100);
  }
  EXPECT_TRUE(VerifyBlackRect(*canvas, kLayerX, kLayerY, kLayerW, kLayerH));

  canvas->drawColor(SK_ColorWHITE);
  {
    LayerSaver layer(*canvas, kLayerX, kLayerY, kLayerW, kLayerH);
    DrawNativeRect(*canvas, kInnerX, kInnerY, kInnerW, kInnerH);
    MakeOpaque(canvas.get(), kInnerX, kInnerY, kInnerW, kInnerH);
  }
  EXPECT_TRUE(VerifyBlackRect(*canvas, kInnerX, kInnerY, kInnerW, kInnerH));

  canvas->drawColor(SK_ColorWHITE);
  {
    LayerSaver layer(*canvas, kLayerX, kLayerY, kLayerW, kLayerH);
    canvas->save();
    AddClip(*canvas, kInnerX, kInnerY, kInnerW, kInnerH);
    DrawNativeRect(*canvas, 0, 0, 100, 100);
    MakeOpaque(canvas.get(), kInnerX, kInnerY, kInnerW, kInnerH);
    canvas->restore();
  }
  EXPECT_TRUE(VerifyBlackRect(*canvas, kInnerX, kInnerY, kInnerW, kInnerH));

  canvas->drawColor(SK_ColorWHITE);
  canvas->save();
  AddClip(*canvas, kInnerX, kInnerY, kInnerW, kInnerH);
  {
    LayerSaver layer(*canvas, kLayerX, kLayerY, kLayerW, kLayerH);
    DrawNativeRect(*canvas, 0, 0, 100, 100);
    MakeOpaque(canvas.get(), 0, 0, 100, 100);
  }
  canvas->restore();
  EXPECT_TRUE(VerifyBlackRect(*canvas, kInnerX, kInnerY, kInnerW, kInnerH));
}

TEST(PlatformCanvas, TranslateLayer) 
{
  std::unique_ptr<SkCanvas> canvas = CreatePlatformCanvas(16, 16, true);

  canvas->drawColor(SK_ColorWHITE);
  canvas->save();
  canvas->translate(1, 1);
  {
    LayerSaver layer(*canvas, kLayerX, kLayerY, kLayerW, kLayerH);
    DrawNativeRect(*canvas, 0, 0, 100, 100);
    MakeOpaque(canvas.get(), 0, 0, 100, 100);
  }
  canvas->restore();
  EXPECT_TRUE(VerifyBlackRect(*canvas, kLayerX + 1, kLayerY + 1, kLayerW, kLayerH));

  canvas->drawColor(SK_ColorWHITE);
  canvas->save();
  canvas->translate(1, 1);
  {
    LayerSaver layer(*canvas, kLayerX, kLayerY, kLayerW, kLayerH);
    DrawNativeRect(*canvas, kInnerX, kInnerY, kInnerW, kInnerH);
    MakeOpaque(canvas.get(), kInnerX, kInnerY, kInnerW, kInnerH);
  }
  canvas->restore();
  EXPECT_TRUE(VerifyBlackRect(*canvas, kInnerX + 1, kInnerY + 1, kInnerW, kInnerH));

  canvas->drawColor(SK_ColorWHITE);
  canvas->save();
  {
    LayerSaver layer(*canvas, kLayerX, kLayerY, kLayerW, kLayerH);
    canvas->translate(1, 1);
    DrawNativeRect(*canvas, kInnerX, kInnerY, kInnerW, kInnerH);
    MakeOpaque(canvas.get(), kInnerX, kInnerY, kInnerW, kInnerH);
  }
  canvas->restore();
  EXPECT_TRUE(VerifyBlackRect(*canvas, kInnerX + 1, kInnerY + 1, kInnerW, kInnerH));

  canvas->drawColor(SK_ColorWHITE);
  canvas->save();
  canvas->translate(1, 1);
  {
    LayerSaver layer(*canvas, kLayerX, kLayerY, kLayerW, kLayerH);
    canvas->drawColor(SK_ColorWHITE);
    canvas->translate(1, 1);
    AddClip(*canvas, kInnerX + 1, kInnerY + 1, kInnerW - 1, kInnerH - 1);
    DrawNativeRect(*canvas, 0, 0, 100, 100);
    MakeOpaque(canvas.get(), kLayerX, kLayerY, kLayerW, kLayerH);
  }
  canvas->restore();
  EXPECT_TRUE(VerifyBlackRect(*canvas, kInnerX + 3, kInnerY + 3, kInnerW - 1, kInnerH - 1));

#if !defined(USE_AURA)
  canvas->drawColor(SK_ColorWHITE);
  canvas->save();
  canvas->translate(1, 1);
  {
    LayerSaver layer(*canvas, kLayerX, kLayerY, kLayerW, kLayerH);
    canvas->drawColor(SK_ColorWHITE);
    canvas->translate(1, 1);

    SkPath path;
    SkRect rect;
    rect.iset(kInnerX - 1, kInnerY - 1, kInnerX + kInnerW, kInnerY + kInnerH);
    const SkScalar kRadius = 2.0;
    path.addRoundRect(rect, kRadius, kRadius);
    canvas->clipPath(path);

    DrawNativeRect(*canvas, 0, 0, 100, 100);
    MakeOpaque(canvas.get(), kLayerX, kLayerY, kLayerW, kLayerH);
  }
  canvas->restore();
  EXPECT_TRUE(VerifyRoundedRect(*canvas, SK_ColorWHITE, SK_ColorBLACK, kInnerX + 1, kInnerY + 1, kInnerW, kInnerH));
#endif
}

}  // namespace skia

#endif  // BUILDFLAG(IS_WIN)
