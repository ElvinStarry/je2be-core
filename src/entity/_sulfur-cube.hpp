#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace je2be {

class SulfurCube {
  SulfurCube() = delete;

public:
  static std::u8string ArchetypeFromJavaItem(std::u8string_view id) {
    std::u8string name(id);
    if (name.starts_with(u8"minecraft:")) {
      name.erase(0, 10);
    }

    if (name.ends_with(u8"_planks") || name.ends_with(u8"_log") || name.ends_with(u8"_wood") || name.ends_with(u8"_stem") || name.ends_with(u8"_hyphae") || name == u8"bamboo_block" || name == u8"stripped_bamboo_block" || name == u8"bamboo_mosaic") {
      return u8"bouncy";
    }
    if (name.ends_with(u8"_wool")) {
      return u8"light";
    }
    if (name.ends_with(u8"_concrete_powder")) {
      return u8"regular";
    }
    if (name.ends_with(u8"_concrete") || name.ends_with(u8"_terracotta")) {
      return u8"slow_bouncy";
    }

    static std::unordered_set<std::u8string> const sExplosive({u8"tnt"});
    static std::unordered_set<std::u8string> const sFastFlat({
        u8"tube_coral_block", u8"brain_coral_block", u8"bubble_coral_block", u8"fire_coral_block", u8"horn_coral_block",
        u8"dead_tube_coral_block", u8"dead_brain_coral_block", u8"dead_bubble_coral_block", u8"dead_fire_coral_block", u8"dead_horn_coral_block",
        u8"sponge", u8"wet_sponge", u8"dried_kelp_block", u8"moss_block", u8"pale_moss_block", u8"resin_block", u8"resin_bricks", u8"chiseled_resin_bricks",
        u8"melon", u8"hay_block", u8"pumpkin", u8"carved_pumpkin", u8"jack_o_lantern", u8"ochre_froglight", u8"pearlescent_froglight", u8"verdant_froglight",
    });
    static std::unordered_set<std::u8string> const sFastSliding({u8"blue_ice", u8"packed_ice", u8"snow_block"});
    static std::unordered_set<std::u8string> const sHighResistance({u8"soul_sand", u8"soul_soil"});
    static std::unordered_set<std::u8string> const sHot({u8"magma_block"});
    static std::unordered_set<std::u8string> const sRegular({
        u8"mud", u8"muddy_mangrove_roots", u8"packed_mud", u8"coal_block", u8"dirt", u8"coarse_dirt", u8"rooted_dirt", u8"podzol", u8"grass_block", u8"clay", u8"bone_block",
    });
    static std::unordered_set<std::u8string> const sSlowBouncy({
        u8"amethyst_block", u8"andesite", u8"basalt", u8"blackstone", u8"bricks", u8"calcite", u8"chiseled_cinnabar", u8"chiseled_deepslate", u8"chiseled_nether_bricks",
        u8"chiseled_polished_blackstone", u8"chiseled_quartz_block", u8"chiseled_red_sandstone", u8"chiseled_sandstone", u8"chiseled_stone_bricks", u8"chiseled_sulfur",
        u8"chiseled_tuff", u8"chiseled_tuff_bricks", u8"cinnabar", u8"cinnabar_bricks", u8"cobbled_deepslate", u8"cobblestone", u8"cracked_deepslate_bricks",
        u8"cracked_deepslate_tiles", u8"cracked_nether_bricks", u8"cracked_polished_blackstone_bricks", u8"cracked_stone_bricks", u8"crimson_nylium", u8"crying_obsidian",
        u8"cut_red_sandstone", u8"cut_sandstone", u8"dark_prismarine", u8"deepslate", u8"deepslate_bricks", u8"deepslate_tiles", u8"diamond_block", u8"diorite", u8"dripstone_block",
        u8"emerald_block", u8"end_stone", u8"end_stone_bricks", u8"gilded_blackstone", u8"glowstone", u8"granite", u8"lapis_block", u8"mossy_cobblestone",
        u8"mossy_stone_bricks", u8"mud_bricks", u8"nether_bricks", u8"netherrack", u8"observer", u8"obsidian", u8"polished_andesite", u8"polished_basalt", u8"polished_blackstone",
        u8"polished_blackstone_bricks", u8"polished_cinnabar", u8"polished_deepslate", u8"polished_diorite", u8"polished_granite", u8"polished_sulfur", u8"polished_tuff",
        u8"prismarine", u8"prismarine_bricks", u8"purpur_block", u8"purpur_pillar", u8"quartz_block", u8"quartz_bricks", u8"nether_quartz_ore", u8"quartz_pillar",
        u8"red_nether_bricks", u8"red_sandstone", u8"redstone_lamp", u8"sandstone", u8"sea_lantern", u8"smooth_basalt", u8"smooth_quartz", u8"smooth_red_sandstone",
        u8"smooth_sandstone", u8"smooth_stone", u8"stone", u8"stone_bricks", u8"sulfur", u8"sulfur_bricks", u8"tuff", u8"tuff_bricks", u8"warped_nylium",
        u8"coal_ore", u8"deepslate_coal_ore", u8"lapis_ore", u8"deepslate_lapis_ore", u8"redstone_ore", u8"deepslate_redstone_ore", u8"diamond_ore", u8"deepslate_diamond_ore",
        u8"emerald_ore", u8"deepslate_emerald_ore",
    });
    static std::unordered_set<std::u8string> const sSlowFlat({
        u8"iron_block", u8"gold_block", u8"raw_copper_block", u8"raw_gold_block", u8"raw_iron_block", u8"netherite_block", u8"ancient_debris",
        u8"gold_ore", u8"deepslate_gold_ore", u8"nether_gold_ore", u8"iron_ore", u8"deepslate_iron_ore", u8"copper_ore", u8"deepslate_copper_ore",
        u8"copper_block", u8"exposed_copper", u8"weathered_copper", u8"oxidized_copper", u8"waxed_copper_block", u8"waxed_exposed_copper", u8"waxed_weathered_copper", u8"waxed_oxidized_copper",
        u8"copper_bulb", u8"exposed_copper_bulb", u8"weathered_copper_bulb", u8"oxidized_copper_bulb", u8"waxed_copper_bulb", u8"waxed_exposed_copper_bulb", u8"waxed_weathered_copper_bulb", u8"waxed_oxidized_copper_bulb",
        u8"cut_copper", u8"exposed_cut_copper", u8"weathered_cut_copper", u8"oxidized_cut_copper", u8"waxed_cut_copper", u8"waxed_exposed_cut_copper", u8"waxed_weathered_cut_copper", u8"waxed_oxidized_cut_copper",
        u8"chiseled_copper", u8"exposed_chiseled_copper", u8"weathered_chiseled_copper", u8"oxidized_chiseled_copper", u8"waxed_chiseled_copper", u8"waxed_exposed_chiseled_copper", u8"waxed_weathered_chiseled_copper", u8"waxed_oxidized_chiseled_copper",
    });
    static std::unordered_set<std::u8string> const sSlowSliding({u8"brown_mushroom_block", u8"red_mushroom_block", u8"mushroom_stem", u8"mycelium", u8"nether_wart_block", u8"warped_wart_block", u8"shroomlight"});
    static std::unordered_set<std::u8string> const sSticky({u8"honeycomb_block"});

    if (sExplosive.contains(name)) return u8"explosive";
    if (sFastFlat.contains(name)) return u8"fast_flat";
    if (sFastSliding.contains(name)) return u8"fast_sliding";
    if (sHighResistance.contains(name)) return u8"high_resistance";
    if (sHot.contains(name)) return u8"hot";
    if (sRegular.contains(name)) return u8"regular";
    if (sSlowBouncy.contains(name)) return u8"slow_bouncy";
    if (sSlowFlat.contains(name)) return u8"slow_flat";
    if (sSlowSliding.contains(name)) return u8"slow_sliding";
    if (sSticky.contains(name)) return u8"sticky";
    return u8"none";
  }
};

} // namespace je2be
