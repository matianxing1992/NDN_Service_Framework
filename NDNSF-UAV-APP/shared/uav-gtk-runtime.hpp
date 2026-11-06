#ifndef NDNSF_UAV_GTK_RUNTIME_HPP
#define NDNSF_UAV_GTK_RUNTIME_HPP

#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>

namespace ndnsf::examples::uav {

/**
 * Finalize process font resources once, on the GUI thread, after all GTK owners
 * and callbacks have drained. No GTK/Pango/Fontconfig operation may follow.
 * A retained font map is an ownership failure; do not invalidate its resources.
 */
inline bool
finishUavGtkRuntime()
{
  for (unsigned i = 0; i < 64 && g_main_context_pending(nullptr); ++i) {
    g_main_context_iteration(nullptr, false);
  }
  if (g_main_context_pending(nullptr)) {
    return false;
  }
  auto* fontMap = pango_cairo_font_map_get_default();
  GWeakRef owner;
  g_weak_ref_init(&owner, G_OBJECT(fontMap));
  if (PANGO_IS_FC_FONT_MAP(fontMap)) {
    pango_fc_font_map_cache_clear(PANGO_FC_FONT_MAP(fontMap));
  }
  pango_cairo_font_map_set_default(nullptr);
  auto* remaining = g_weak_ref_get(&owner);
  g_weak_ref_clear(&owner);
  if (remaining) {
    g_object_unref(remaining);
    return false;
  }
  FcFini();
  return true;
}

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_GTK_RUNTIME_HPP
