#include "extract_level.h"

#include <set>
#include <thread>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/SimpleThreadGroup.h"
#include "common/util/compress.h"
#include "common/util/string_util.h"

#include "decompiler/level_extractor/BspHeader.h"
#include "decompiler/level_extractor/extract_collide_frags.h"
#include "decompiler/level_extractor/extract_merc.h"
#include "decompiler/level_extractor/extract_shrub.h"
#include "decompiler/level_extractor/extract_tfrag.h"
#include "decompiler/level_extractor/extract_tie.h"
#include "decompiler/level_extractor/fr3_to_gltf.h"

namespace decompiler {

/*!
 * Look through files in a DGO and find the bsp-header file (the level)
 */
std::optional<ObjectFileRecord> get_bsp_file(const std::vector<ObjectFileRecord>& records,
                                             const std::string& dgo_name) {
  std::optional<ObjectFileRecord> result;
  bool found = false;
  for (auto& file : records) {
    if (file.name.length() > 4 && file.name.substr(file.name.length() - 4) == "-vis") {
      ASSERT(!found);
      found = true;
      result = file;
    }
  }

  if (!result) {
    if (str_util::ends_with(dgo_name, ".DGO") || str_util::ends_with(dgo_name, ".CGO")) {
      auto expected_name = dgo_name.substr(0, dgo_name.length() - 4);
      for (auto& c : expected_name) {
        c = tolower(c);
      }
      if (!records.empty() && expected_name == records.back().name) {
        return records.back();
      }
    }
  }
  return result;
}

/*!
 * Make sure a file is a valid bsp-header.
 */
bool is_valid_bsp(const decompiler::LinkedObjectFile& file) {
  if (file.segments != 1) {
    lg::error("Got {} segments, but expected 1", file.segments);
    return false;
  }

  auto& first_word = file.words_by_seg.at(0).at(0);
  if (first_word.kind() != decompiler::LinkedWord::TYPE_PTR) {
    lg::error("Expected the first word to be a type pointer, but it wasn't.");
    return false;
  }

  if (first_word.symbol_name() != "bsp-header") {
    lg::error("Expected to get a bsp-header, but got {} instead.", first_word.symbol_name());
    return false;
  }

  return true;
}

void add_all_textures_from_level(tfrag3::Level& lev,
                                 const std::string& level_name,
                                 const TextureDB& tex_db) {
  ASSERT(lev.textures.empty());
  const auto& level_it = tex_db.texture_ids_per_level.find(level_name);
  if (level_it != tex_db.texture_ids_per_level.end()) {
    for (auto id : level_it->second) {
      const auto& tex = tex_db.textures.at(id);
      lev.textures.emplace_back();
      auto& new_tex = lev.textures.back();
      new_tex.combo_id = id;
      new_tex.w = tex.w;
      new_tex.h = tex.h;
      new_tex.debug_tpage_name = tex_db.tpage_names.at(tex.page);
      new_tex.debug_name = new_tex.debug_tpage_name + tex.name;
      new_tex.data = tex.rgba_bytes;
      new_tex.combo_id = id;
      new_tex.load_to_pool = true;
    }
  }
}

void confirm_textures_identical(const TextureDB& tex_db) {
  std::unordered_map<std::string, std::vector<u32>> tex_dupl;
  for (auto& tex : tex_db.textures) {
    auto name = tex_db.tpage_names.at(tex.second.page) + tex.second.name;
    auto it = tex_dupl.find(name);
    if (it == tex_dupl.end()) {
      tex_dupl.insert({name, tex.second.rgba_bytes});
    } else {
      bool ok = it->second == tex.second.rgba_bytes;
      if (!ok) {
        ASSERT_MSG(false, fmt::format("BAD duplicate: {} {} vs {}", name,
                                      tex.second.rgba_bytes.size(), it->second.size()));
      }
    }
  }
}

void extract_art_groups_from_level(const ObjectFileDB& db,
                                   const TextureDB& tex_db,
                                   const std::vector<level_tools::TextureRemap>& tex_remap,
                                   const std::string& dgo_name,
                                   tfrag3::Level& level_data) {
  const auto& files = db.obj_files_by_dgo.at(dgo_name);
  for (const auto& file : files) {
    if (file.name.length() > 3 && !file.name.compare(file.name.length() - 3, 3, "-ag")) {
      const auto& ag_file = db.lookup_record(file);
      extract_merc(ag_file, tex_db, db.dts, tex_remap, level_data, false, db.version());
    }
  }
}

std::vector<level_tools::TextureRemap> extract_bsp_from_level(const ObjectFileDB& db,
                                                              const TextureDB& tex_db,
                                                              const std::string& dgo_name,
                                                              const DecompileHacks& hacks,
                                                              bool extract_collision,
                                                              tfrag3::Level& level_data) {
  auto bsp_rec = get_bsp_file(db.obj_files_by_dgo.at(dgo_name), dgo_name);
  if (!bsp_rec) {
    lg::warn("Skipping extract for {} because the BSP file was not found", dgo_name);
    return {};
  }
  std::string level_name = bsp_rec->name.substr(0, bsp_rec->name.length() - 4);

  lg::info("Processing level {} ({})", dgo_name, level_name);
  const auto& bsp_file = db.lookup_record(*bsp_rec);
  bool ok = is_valid_bsp(bsp_file.linked_data);
  ASSERT(ok);

  level_tools::DrawStats draw_stats;
  // draw_stats.debug_print_dma_data = true;
  level_tools::BspHeader bsp_header;
  bsp_header.read_from_file(bsp_file.linked_data, db.dts, &draw_stats, db.version());
  ASSERT((int)bsp_header.drawable_tree_array.trees.size() == bsp_header.drawable_tree_array.length);

  /*
  level_tools::PrintSettings settings;
  settings.expand_collide = true;
  lg::print("{}\n", bsp_header.print(settings));
   */

  const std::set<std::string> tfrag_trees = {
      "drawable-tree-tfrag",        "drawable-tree-trans-tfrag",       "drawable-tree-tfrag-trans",
      "drawable-tree-dirt-tfrag",   "drawable-tree-tfrag-water",       "drawable-tree-ice-tfrag",
      "drawable-tree-lowres-tfrag", "drawable-tree-lowres-trans-tfrag"};
  int i = 0;

  std::vector<const level_tools::DrawableTreeInstanceTie*> all_ties;
  for (auto& draw_tree : bsp_header.drawable_tree_array.trees) {
    auto as_tie_tree = dynamic_cast<level_tools::DrawableTreeInstanceTie*>(draw_tree.get());
    if (as_tie_tree) {
      all_ties.push_back(as_tie_tree);
    }
  }

  bool got_collide = false;
  for (auto& draw_tree : bsp_header.drawable_tree_array.trees) {
    if (tfrag_trees.count(draw_tree->my_type())) {
      auto as_tfrag_tree = dynamic_cast<level_tools::DrawableTreeTfrag*>(draw_tree.get());
      ASSERT(as_tfrag_tree);
      std::vector<std::pair<int, int>> expected_missing_textures;
      auto it = hacks.missing_textures_by_level.find(level_name);
      if (it != hacks.missing_textures_by_level.end()) {
        expected_missing_textures = it->second;
      }
      bool atest_disable_flag = false;
      if (db.version() == GameVersion::Jak2) {
        if (bsp_header.texture_flags[0] & 1) {
          atest_disable_flag = true;
        }
      }
      extract_tfrag(as_tfrag_tree, fmt::format("{}-{}", dgo_name, i++),
                    bsp_header.texture_remap_table, tex_db, expected_missing_textures, level_data,
                    false, level_name, atest_disable_flag);
    } else if (draw_tree->my_type() == "drawable-tree-instance-tie") {
      auto as_tie_tree = dynamic_cast<level_tools::DrawableTreeInstanceTie*>(draw_tree.get());
      ASSERT(as_tie_tree);
      extract_tie(as_tie_tree, fmt::format("{}-{}-tie", dgo_name, i++),
                  bsp_header.texture_remap_table, tex_db, level_data, false, db.version());
    } else if (draw_tree->my_type() == "drawable-tree-instance-shrub") {
      auto as_shrub_tree =
          dynamic_cast<level_tools::shrub_types::DrawableTreeInstanceShrub*>(draw_tree.get());
      ASSERT(as_shrub_tree);
      extract_shrub(as_shrub_tree, fmt::format("{}-{}-shrub", dgo_name, i++),
                    bsp_header.texture_remap_table, tex_db, {}, level_data, false, db.version());
    } else if (draw_tree->my_type() == "drawable-tree-collide-fragment" && extract_collision) {
      auto as_collide_frags =
          dynamic_cast<level_tools::DrawableTreeCollideFragment*>(draw_tree.get());
      ASSERT(as_collide_frags);
      ASSERT(!got_collide);
      got_collide = true;
      extract_collide_frags(as_collide_frags, all_ties, fmt::format("{}-{}-collide", dgo_name, i++),
                            level_data, false);
    } else {
      lg::print("  unsupported tree {}\n", draw_tree->my_type());
    }
  }
  level_data.level_name = level_name;

  return bsp_header.texture_remap_table;
}

/*!
 * Extract stuff found in GAME.CGO.
 * Even though GAME.CGO isn't technically a level, the decompiler/loader treat it like one,
 * but the bsp stuff is just empty. It will contain only textures/art groups.
 */
void extract_common(const ObjectFileDB& db,
                    const TextureDB& tex_db,
                    const std::string& dgo_name,
                    bool dump_levels,
                    const fs::path& output_folder) {
  if (db.obj_files_by_dgo.count(dgo_name) == 0) {
    lg::warn("Skipping common extract for {} because the DGO was not part of the input", dgo_name);
    return;
  }

  if (tex_db.textures.size() == 0) {
    lg::warn("Skipping common extract because there were no textures in the input");
    return;
  }

  confirm_textures_identical(tex_db);

  tfrag3::Level tfrag_level;
  add_all_textures_from_level(tfrag_level, dgo_name, tex_db);
  extract_art_groups_from_level(db, tex_db, {}, dgo_name, tfrag_level);

  Serializer ser;
  tfrag_level.serialize(ser);
  auto compressed =
      compression::compress_zstd(ser.get_save_result().first, ser.get_save_result().second);

  lg::info("stats for {}", dgo_name);
  print_memory_usage(tfrag_level, ser.get_save_result().second);
  lg::info("compressed: {} -> {} ({:.2f}%)", ser.get_save_result().second, compressed.size(),
           100.f * compressed.size() / ser.get_save_result().second);
  file_util::write_binary_file(
      output_folder / fmt::format("{}.fr3", dgo_name.substr(0, dgo_name.length() - 4)),
      compressed.data(), compressed.size());

  if (dump_levels) {
    auto file_path = file_util::get_jak_project_dir() / "glb_out" / "common.glb";
    file_util::create_dir_if_needed_for_file(file_path);
    save_level_foreground_as_gltf(tfrag_level, file_path);
  }
}

void extract_from_level(const ObjectFileDB& db,
                        const TextureDB& tex_db,
                        const std::string& dgo_name,
                        const DecompileHacks& hacks,
                        bool dump_level,
                        bool extract_collision,
                        const fs::path& output_folder) {
  if (db.obj_files_by_dgo.count(dgo_name) == 0) {
    lg::warn("Skipping extract for {} because the DGO was not part of the input", dgo_name);
    return;
  }
  tfrag3::Level level_data;
  add_all_textures_from_level(level_data, dgo_name, tex_db);

  // the bsp header file data
  auto tex_remap =
      extract_bsp_from_level(db, tex_db, dgo_name, hacks, extract_collision, level_data);
  extract_art_groups_from_level(db, tex_db, tex_remap, dgo_name, level_data);

  //If the dgo is not snowy, then add snowy assets for flutflut
  if (dgo_name != "SNO.DGO" && db.obj_files_by_dgo.count(dgo_name) == 0) {
    lg::warn("Skipping adding {} because we are in Jak 2 mode", dgo_name);
    const std::string local_dgo_name = "SNO.DGO"; 
    extract_art_groups_from_level(db, tex_db, extract_bsp_from_level(db, tex_db, local_dgo_name, hacks, extract_collision, level_data), local_dgo_name, level_data);
    return;
  }
  
  //If the dgo is not misty, then add misty assets for racer
  if (dgo_name != "MIS.DGO" && db.obj_files_by_dgo.count(dgo_name) == 0) {
    lg::warn("Skipping adding {} because we are in Jak 2 mode", dgo_name);
    const std::string local_dgo_name = "MIS.DGO"; 
    extract_art_groups_from_level(db, tex_db, extract_bsp_from_level(db, tex_db, local_dgo_name, hacks, extract_collision, level_data), local_dgo_name, level_data);
    return;
  }
  
  Serializer ser;
  level_data.serialize(ser);
  auto compressed =
      compression::compress_zstd(ser.get_save_result().first, ser.get_save_result().second);
  lg::info("stats for {}", dgo_name);
  print_memory_usage(level_data, ser.get_save_result().second);
  lg::info("compressed: {} -> {} ({:.2f}%)", ser.get_save_result().second, compressed.size(),
           100.f * compressed.size() / ser.get_save_result().second);
  file_util::write_binary_file(
      output_folder / fmt::format("{}.fr3", dgo_name.substr(0, dgo_name.length() - 4)),
      compressed.data(), compressed.size());

file_util::write_binary_file(output_folder / fmt::format("ATE.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("ATO.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CAB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CAP.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CAS.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CASCITY.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CASEXT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CFA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CFB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CGA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CGB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CGC.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CIA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CIB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CMA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CMB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("COA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("COB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CPA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CPO.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CTA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CTB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CTC.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CTYASHA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CTYKORA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("CWI.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("D3A.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("D3B.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("DEMO.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("DG1.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("DMI.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("DRB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("DRI.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("DRILLMTN.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("FDA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("FDB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("FEA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("FEB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("FOB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("FOR.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("FORDUMPC.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("FORDUMPD.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("FRA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("FRB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("GAME.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("GARAGE.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("GGA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("HALFPIPE.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("HIDEOUT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("HIPHOG.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("INTROCST.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("KIOSK.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LASHGRD.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LASHTHRN.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LBBUSH.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LBOMBBOT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LBRNERMK.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LCGUARD.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LCITYLOW.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LDJAKBRN.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LERLCHAL.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LERLTESS.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LERROL.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LGARCSTA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LGUARD.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LHELLDOG.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LHIPOUT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LINTCSTB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LJAKDAX.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LJKDXASH.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LKEIRIFT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LKIDDOGE.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LMEETBRT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LOUTCSTB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LPACKAGE.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LPORTRUN.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LPOWER.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LPROTECT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LPRSNCST.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LPRTRACE.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LRACEBB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LRACEBF.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LRACECB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LRACECF.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LRACEDB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LRACEDF.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LRACELIT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LSACK.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LSAMERGD.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LSHUTTLE.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LSMYSBRT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LTENTOB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LTENTOUT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LTESS.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LTHRNOUT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LTRNKRKD.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LTRNTESS.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LTRNYSAM.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LWHACK.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LWIDEA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LWIDEB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LWIDEC.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LWIDESTA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LYSAMSAM.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("LYSKDCD.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("MCN.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("MTN.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("MTX.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("NEB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("NES.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("NESTT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("ONINTENT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("ORACLE.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("OUTROCST.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("PAC.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("PAE.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("PALBOSS.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("PALOUT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("PAR.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("PAS.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("PORTWALL.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("PRI.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("RUI.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("SAG.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("SEB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("SEW.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("SKA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("STA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("STADBLMP.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("STB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("STC.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("STD.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("STR.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("SWB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("SWE.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("TBO.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("THR.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("TITLE.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("TOA.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("TOB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("TOC.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("TOD.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("TOE.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("TOMBEXT.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("TSZ.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("UNB.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("UND.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("VI1.fr3"), compressed.data(), compressed.size());
file_util::write_binary_file(output_folder / fmt::format("VIN.fr3"), compressed.data(), compressed.size());


  file_util::write_binary_file(output_folder / fmt::format("BEA.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("CIT.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("DAR.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("DEM.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("FIC.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("FIN.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("INT.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("JUB.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("JUN.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("LAV.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("MAI.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("MIS.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("OGR.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("ROB.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("ROL.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("SNO.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("SUB.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("SUN.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("SWA.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("TIT.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("TRA.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("TSZ.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("VI1.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("VI2.fr3"), compressed.data(), compressed.size());
  file_util::write_binary_file(output_folder / fmt::format("VI3.fr3"), compressed.data(), compressed.size());


  if (dump_level) {
    auto back_file_path = file_util::get_jak_project_dir() / "glb_out" /
                          fmt::format("{}_background.glb", level_data.level_name);
    file_util::create_dir_if_needed_for_file(back_file_path);
    save_level_background_as_gltf(level_data, back_file_path);
    auto fore_file_path = file_util::get_jak_project_dir() / "glb_out" /
                          fmt::format("{}_foreground.glb", level_data.level_name);
    file_util::create_dir_if_needed_for_file(fore_file_path);
    save_level_foreground_as_gltf(level_data, fore_file_path);
  }
}

void extract_all_levels(const ObjectFileDB& db,
                        const TextureDB& tex_db,
                        const std::vector<std::string>& dgo_names,
                        const std::string& common_name,
                        const DecompileHacks& hacks,
                        bool debug_dump_level,
                        bool extract_collision,
                        const fs::path& output_path) {
  extract_common(db, tex_db, common_name, debug_dump_level, output_path);
  SimpleThreadGroup threads;
  threads.run(
      [&](int idx) {
        extract_from_level(db, tex_db, dgo_names[idx], hacks, debug_dump_level, extract_collision,
                           output_path);
      },
      dgo_names.size());
  threads.join();
}

}  // namespace decompiler
