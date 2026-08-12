// HouseDecor.db2 catalog implementation.
// Copyright (C) 2026 WarcraftXL
// GPL-3.0-or-later.

#include "Catalog.hpp"

#include "ExtensionApi.hpp"
#include "FreeBuildCamera.hpp"
#include "Placement.hpp"
#include "ImGuiHostExt.hpp"
#include "TerrainDeform.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <windows.h>
#include <d3d9.h>
#include "imgui.h"

namespace wxl_housing
{
    namespace
    {
        // 12.1.0.68914 (WOWSTATIC_12_1_0_68914). The CSV has 19 columns because
        // InitialRotation is a three-element physical field.
        const WXL_Db2Field kFields121[] = {
            { "Name_lang",              1 }, // string - unreadable via the v1 uint32 Value()
            { "InitialRotation",        3 }, // float[3]
            { "ID",                     1 },
            { "GameObjectID",           1 },
            { "Flags",                  1 },
            { "Type",                   1 },
            { "ModelType",              1 },
            { "ModelFileDataID",        1 },
            { "ThumbnailFileDataID",    1 },
            { "WeightCost",             1 },
            { "ItemID",                 1 },
            { "InitialScale",           1 }, // float
            { "FirstTimeAcquisitionXP", 1 },
            { "OrderIndex",             1 },
            { "Field_12_0_0_63534_015", 1 },
            { "StartingQuantity",       1 },
            { "UiModelSceneID",         1 },
        };
        constexpr uint32_t kFieldCount121 = sizeof(kFields121) / sizeof(kFields121[0]);

        // 12.0.7.68974 adds one physical field immediately after ID. Supporting both
        // layouts lets a stock current Retail DB2 work as well as the supplied 12.1 set.
        const WXL_Db2Field kFields1207[] = {
            { "Name_lang",              1 },
            { "InitialRotation",        3 },
            { "ID",                     1 },
            { "Field_12_0_0_63534_003", 1 },
            { "GameObjectID",           1 },
            { "Flags",                  1 },
            { "Type",                   1 },
            { "ModelType",              1 },
            { "ModelFileDataID",        1 },
            { "ThumbnailFileDataID",    1 },
            { "WeightCost",             1 },
            { "ItemID",                 1 },
            { "InitialScale",           1 },
            { "FirstTimeAcquisitionXP", 1 },
            { "OrderIndex",             1 },
            { "Field_12_0_0_63534_015", 1 },
            { "StartingQuantity",       1 },
            { "UiModelSceneID",         1 },
        };
        constexpr uint32_t kFieldCount1207 = sizeof(kFields1207) / sizeof(kFields1207[0]);

        constexpr uint32_t kLayout121  = 0x6A051268u;
        constexpr uint32_t kLayout1207 = 0xEAA015F9u;

        const char* TypeName(uint32_t t)
        {
            switch (t)
            {
                case 0: return "other";
                case 1: return "free-standing";
                case 2: return "wall";
                case 3: return "ceiling";
                case 4: return "floor / rug";
                default: return "other";
            }
        }

        const char* ModelTypeName(uint32_t t)
        {
            switch (t)
            {
                case 1: return "M2";
                case 2: return "WMO";
                default: return "none";
            }
        }

        bool EndsWithNoCase(const std::string& value, const char* suffix)
        {
            const size_t n = std::strlen(suffix);
            return value.size() >= n &&
                _stricmp(value.c_str() + value.size() - n, suffix) == 0;
        }

        bool ContainsNoCase(const std::string& value, const char* needle)
        {
            if (!needle || !*needle) return true;
            return std::search(value.begin(), value.end(), needle, needle + std::strlen(needle),
                [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                }) != value.end();
        }

        bool IsDntName(const std::string& name)
        {
            return name.rfind("[DNT]", 0) == 0 || name.rfind("{DNT]", 0) == 0;
        }

        enum RetailCategory
        {
            kCategoryAll = 0,
            kCategoryFurnishings,
            kCategoryAccents,
            kCategoryStructural,
            kCategoryLighting,
            kCategoryNature,
            kCategoryFunctional,
            kCategoryCustom,
            kCategoryMisc,
        };

        // Optional local Retail art extracted from the user's installed client.
        // These high cache keys cannot collide with real FileDataIDs.
        constexpr uint32_t kUiTextureCategoryNavigation = 0xFFF00001u;
        constexpr uint32_t kUiTextureCatalogPanel       = 0xFFF00002u;

        const char* CategoryName(int category)
        {
            static const char* names[] = {
                "All", "Furnishings", "Accents", "Structural",
                "Lighting", "Nature", "Functional", "Custom Props", "Misc"
            };
            return category >= kCategoryAll && category <= kCategoryMisc
                ? names[category] : names[kCategoryMisc];
        }

        bool HasAnyToken(const DecorRow& row, const char* const* tokens, size_t count)
        {
            for (size_t i = 0; i < count; ++i)
                if (ContainsNoCase(row.name, tokens[i]) || ContainsNoCase(row.modelPath, tokens[i]))
                    return true;
            return false;
        }

        int CategoryFor(const DecorRow& row)
        {
            if (row.custom) return kCategoryCustom;
            static const char* lighting[] = {
                "light", "lamp", "lantern", "candle", "torch", "brazier", "fireplace", "sconce"
            };
            static const char* nature[] = {
                "tree", "plant", "flower", "grass", "bush", "foliage", "mushroom", "vine", "boulder"
            };
            static const char* structural[] = {
                "wall", "pillar", "column", "door", "window", "fence", "gate", "roof", "beam", "gazebo"
            };
            static const char* functional[] = {
                "stove", "oven", "cauldron", "cookpot", "anvil", "craft", "workbench", "forge"
            };
            static const char* furnishings[] = {
                "chair", "table", "desk", "bed", "shelf", "cabinet", "couch", "sofa", "bench", "stool", "rug"
            };
            static const char* accents[] = {
                "book", "banner", "curtain", "painting", "portrait", "statue", "ornament", "vase", "trophy", "food"
            };
            if (HasAnyToken(row, lighting, sizeof lighting / sizeof lighting[0])) return kCategoryLighting;
            if (HasAnyToken(row, nature, sizeof nature / sizeof nature[0])) return kCategoryNature;
            if (HasAnyToken(row, structural, sizeof structural / sizeof structural[0])) return kCategoryStructural;
            if (HasAnyToken(row, functional, sizeof functional / sizeof functional[0])) return kCategoryFunctional;
            if (HasAnyToken(row, furnishings, sizeof furnishings / sizeof furnishings[0])) return kCategoryFurnishings;
            if (HasAnyToken(row, accents, sizeof accents / sizeof accents[0])) return kCategoryAccents;
            return row.type == 4 ? kCategoryAccents : kCategoryMisc;
        }

        const char* PlacementStatus(const DecorRow& r)
        {
            if (r.placeable) return "Ready to place";
            if (r.modelType == 2) return "WMO - catalog only";
            if (r.custom && !r.assetInstalled) return "Custom model parts not installed";
            if (!r.assetInstalled && !r.modelPath.empty()) return "Model asset not installed";
            if (!r.modelFdid) return "No model";
            return "Model unavailable";
        }

        bool LocalModelExists(const std::string& path)
        {
            if (path.empty()) return false;
            if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
            const std::string patch9 = "Data/Patch-9.MPQ/" + path;
            return GetFileAttributesA(patch9.c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        bool LocalModelsExist(const DecorRow& row)
        {
            if (row.modelParts.empty()) return LocalModelExists(row.modelPath);
            return std::all_of(row.modelParts.begin(), row.modelParts.end(),
                [](const std::string& path) { return LocalModelExists(path); });
        }

        bool IsPlaceableM2(const DecorRow& row)
        {
            const bool hasIdentity = row.custom ? !row.modelPath.empty() : row.modelFdid != 0;
            if (!hasIdentity || row.modelType != 1 || !row.assetInstalled ||
                !EndsWithNoCase(row.modelPath, ".m2"))
                return false;

            return std::all_of(row.modelParts.begin(), row.modelParts.end(),
                [](const std::string& path) { return EndsWithNoCase(path, ".m2"); });
        }

        struct ManifestData
        {
            std::string schema;
            uint32_t schemaVersion = 0;
            std::string packageId;
            uint32_t rowId = 0;
            std::string name;
            std::string category;
            std::string kind;
            std::vector<std::string> models;
            float scale = 1.0f;
            float spawnDistance = 4.0f;
            std::string gameRoot;
            std::string primary;
            std::string thumbnail;
            std::string attribution;
            std::string attributionFile;
        };

        // decor.json is deliberately parsed here instead of adding a DLL dependency.
        // The parser accepts ordinary JSON escapes (including surrogate pairs), ignores
        // unknown extension fields, and rejects duplicate/ambiguous manifest fields.
        class ManifestParser
        {
        public:
            explicit ManifestParser(const std::string& source) : source_(source) {}

            bool Parse(ManifestData& output, std::string& error)
            {
                if (source_.size() >= 3 &&
                    static_cast<unsigned char>(source_[0]) == 0xEF &&
                    static_cast<unsigned char>(source_[1]) == 0xBB &&
                    static_cast<unsigned char>(source_[2]) == 0xBF)
                    pos_ = 3;

                SkipWhitespace();
                if (!Consume('{')) return Finish(error, "root must be an object");

                std::unordered_set<std::string> rawKeys;
                SkipWhitespace();
                if (!Consume('}'))
                {
                    for (;;)
                    {
                        std::string key;
                        if (!ParseString(key)) return CopyError(error);
                        if (!rawKeys.insert(key).second)
                            return Finish(error, "duplicate key '" + key + "'");
                        SkipWhitespace();
                        if (!Consume(':')) return Finish(error, "expected ':' after key");
                        SkipWhitespace();

                        const Field field = IdentifyField(key);
                        if (field != kUnknown)
                        {
                            if (seen_[field])
                                return Finish(error, "duplicate alias for field '" + key + "'");
                            seen_[field] = true;
                        }

                        double number = 0.0;
                        switch (field)
                        {
                            case kSchema:
                                if (!ParseString(output.schema)) return CopyError(error);
                                break;
                            case kSchemaVersion:
                                if (!ParseNumber(number)) return CopyError(error);
                                if (number < 1.0 || number > static_cast<double>((std::numeric_limits<uint32_t>::max)()) ||
                                    std::floor(number) != number)
                                    return Finish(error, "schemaVersion must be a positive integer");
                                output.schemaVersion = static_cast<uint32_t>(number);
                                break;
                            case kPackageId:
                                if (pos_ < source_.size() && source_[pos_] == '"')
                                {
                                    if (!ParseString(output.packageId)) return CopyError(error);
                                }
                                else
                                {
                                    if (!Mark(kRowId, "id") || !ParseNumber(number)) return CopyError(error);
                                    if (number < 1.0 ||
                                        number > static_cast<double>((std::numeric_limits<uint32_t>::max)()) ||
                                        std::floor(number) != number)
                                        return Finish(error, "numeric id must be an integer from 1 through UINT32_MAX");
                                    output.rowId = static_cast<uint32_t>(number);
                                }
                                break;
                            case kCatalog:
                                if (!ParseCatalog(output)) return CopyError(error);
                                break;
                            case kPlacement:
                                if (!ParsePlacement(output)) return CopyError(error);
                                break;
                            case kContent:
                                if (!ParseContent(output)) return CopyError(error);
                                break;
                            case kLicense:
                                if (!ParseLicense(output)) return CopyError(error);
                                break;
                            case kRowId:
                                if (!ParseNumber(number)) return CopyError(error);
                                if (!std::isfinite(number) || number < 1.0 ||
                                    number > static_cast<double>((std::numeric_limits<uint32_t>::max)()) ||
                                    std::floor(number) != number)
                                    return Finish(error, "row id must be an integer from 1 through UINT32_MAX");
                                output.rowId = static_cast<uint32_t>(number);
                                break;
                            case kName:
                                if (!ParseString(output.name)) return CopyError(error);
                                break;
                            case kCategory:
                                if (!ParseString(output.category)) return CopyError(error);
                                break;
                            case kModels:
                                if (!ParseStringList(output.models)) return CopyError(error);
                                break;
                            case kScale:
                                if (!ParseNumber(number)) return CopyError(error);
                                output.scale = static_cast<float>(number);
                                break;
                            case kSpawnDistance:
                                if (!ParseNumber(number)) return CopyError(error);
                                output.spawnDistance = static_cast<float>(number);
                                break;
                            case kThumbnail:
                                if (!ParseString(output.thumbnail)) return CopyError(error);
                                break;
                            case kAttribution:
                                if (!ParseString(output.attribution)) return CopyError(error);
                                break;
                            case kUnknown:
                                if (!SkipValue(0)) return CopyError(error);
                                break;
                            default:
                                return Finish(error, "internal parser field error");
                        }

                        SkipWhitespace();
                        if (Consume('}')) break;
                        if (!Consume(',')) return Finish(error, "expected ',' or '}'");
                        SkipWhitespace();
                    }
                }

                SkipWhitespace();
                if (pos_ != source_.size()) return Finish(error, "trailing content after root object");
                if (seen_[kSchema])
                {
                    if (output.schema != "wxl.decor/1") return Finish(error, "unsupported schema '" + output.schema + "'");
                    if (!seen_[kSchemaVersion] || output.schemaVersion != 1)
                        return Finish(error, "wxl.decor/1 requires schemaVersion 1");
                    if (!seen_[kPackageId] || output.packageId.empty())
                        return Finish(error, "wxl.decor/1 requires a package id");
                    if (!seen_[kCatalog] || !seen_[kPlacement] || !seen_[kContent])
                        return Finish(error, "wxl.decor/1 requires catalog, placement and content objects");
                    if (!seen_[kGameRoot]) return Finish(error, "content.gameRoot is required");
                    if (!seen_[kScale] || !seen_[kSpawnDistance])
                        return Finish(error, "placement.initialScale and placement.spawnDistance are required");
                }
                if (!seen_[kRowId]) return Finish(error, "missing required catalog.rowId/row_id");
                if (!seen_[kName]) return Finish(error, "missing required catalog.displayName/name");
                if (!seen_[kCategory]) return Finish(error, "missing required catalog.category/category");
                if (!seen_[kModels]) return Finish(error, "missing required content.parts/model(s)");
                return true;
            }

        private:
            enum Field
            {
                kUnknown = 0,
                kSchema,
                kSchemaVersion,
                kPackageId,
                kCatalog,
                kPlacement,
                kContent,
                kLicense,
                kRowId,
                kName,
                kCategory,
                kModels,
                kScale,
                kSpawnDistance,
                kGameRoot,
                kAttributionFile,
                kThumbnail,
                kAttribution,
                kFieldCount,
            };

            static Field IdentifyField(const std::string& key)
            {
                if (key == "schema") return kSchema;
                if (key == "schemaVersion") return kSchemaVersion;
                if (key == "id") return kPackageId;
                if (key == "catalog") return kCatalog;
                if (key == "placement") return kPlacement;
                if (key == "content") return kContent;
                if (key == "license") return kLicense;
                if (key == "row_id" || key == "rowId") return kRowId;
                if (key == "name") return kName;
                if (key == "category") return kCategory;
                if (key == "model" || key == "model_path" || key == "modelPath" ||
                    key == "models" || key == "model_paths" || key == "modelParts") return kModels;
                if (key == "scale" || key == "initial_scale" || key == "initialScale") return kScale;
                if (key == "spawn_distance" || key == "spawnDistance") return kSpawnDistance;
                if (key == "thumbnail" || key == "thumbnail_path" || key == "thumbPath") return kThumbnail;
                if (key == "attribution") return kAttribution;
                return kUnknown;
            }

            bool Mark(Field field, const char* path)
            {
                if (seen_[field]) return Fail(std::string("duplicate field '") + path + "'");
                seen_[field] = true;
                return true;
            }

            bool ParseCatalog(ManifestData& output)
            {
                if (!Consume('{')) return Fail("catalog must be an object");
                std::unordered_set<std::string> keys;
                SkipWhitespace();
                if (Consume('}')) return true;
                for (;;)
                {
                    std::string key;
                    if (!ParseString(key)) return false;
                    if (!keys.insert(key).second) return Fail("duplicate catalog key '" + key + "'");
                    SkipWhitespace();
                    if (!Consume(':')) return Fail("expected ':' in catalog");
                    SkipWhitespace();
                    if (key == "rowId")
                    {
                        if (!Mark(kRowId, "catalog.rowId")) return false;
                        double number = 0.0;
                        if (!ParseNumber(number)) return false;
                        if (number < 1.0 ||
                            number > static_cast<double>((std::numeric_limits<uint32_t>::max)()) ||
                            std::floor(number) != number)
                            return Fail("catalog.rowId must be an integer from 1 through UINT32_MAX");
                        output.rowId = static_cast<uint32_t>(number);
                    }
                    else if (key == "displayName")
                    {
                        if (!Mark(kName, "catalog.displayName") || !ParseString(output.name)) return false;
                    }
                    else if (key == "category")
                    {
                        if (!Mark(kCategory, "catalog.category") || !ParseString(output.category)) return false;
                    }
                    else if (key == "kind")
                    {
                        if (!ParseString(output.kind)) return false;
                    }
                    else if (!SkipValue(0)) return false;

                    SkipWhitespace();
                    if (Consume('}')) return true;
                    if (!Consume(',')) return Fail("expected ',' or '}' in catalog");
                    SkipWhitespace();
                }
            }

            bool ParsePlacement(ManifestData& output)
            {
                if (!Consume('{')) return Fail("placement must be an object");
                std::unordered_set<std::string> keys;
                SkipWhitespace();
                if (Consume('}')) return true;
                for (;;)
                {
                    std::string key;
                    if (!ParseString(key)) return false;
                    if (!keys.insert(key).second) return Fail("duplicate placement key '" + key + "'");
                    SkipWhitespace();
                    if (!Consume(':')) return Fail("expected ':' in placement");
                    SkipWhitespace();
                    double number = 0.0;
                    if (key == "spawnDistance")
                    {
                        if (!Mark(kSpawnDistance, "placement.spawnDistance") || !ParseNumber(number)) return false;
                        output.spawnDistance = static_cast<float>(number);
                    }
                    else if (key == "initialScale")
                    {
                        if (!Mark(kScale, "placement.initialScale") || !ParseNumber(number)) return false;
                        output.scale = static_cast<float>(number);
                    }
                    else if (!SkipValue(0)) return false;

                    SkipWhitespace();
                    if (Consume('}')) return true;
                    if (!Consume(',')) return Fail("expected ',' or '}' in placement");
                    SkipWhitespace();
                }
            }

            bool ParseParts(std::vector<std::string>& models)
            {
                if (!Consume('[')) return Fail("content.parts must be an array");
                models.clear();
                SkipWhitespace();
                if (Consume(']')) return true;
                for (;;)
                {
                    if (!Consume('{')) return Fail("each content.parts entry must be an object");
                    std::unordered_set<std::string> keys;
                    std::string model;
                    SkipWhitespace();
                    if (!Consume('}'))
                    {
                        for (;;)
                        {
                            std::string key;
                            if (!ParseString(key)) return false;
                            if (!keys.insert(key).second)
                                return Fail("duplicate content.parts key '" + key + "'");
                            SkipWhitespace();
                            if (!Consume(':')) return Fail("expected ':' in content.parts entry");
                            SkipWhitespace();
                            if (key == "model")
                            {
                                if (!model.empty()) return Fail("duplicate content.parts.model");
                                if (!ParseString(model)) return false;
                            }
                            else if (!SkipValue(0)) return false;
                            SkipWhitespace();
                            if (Consume('}')) break;
                            if (!Consume(',')) return Fail("expected ',' or '}' in content.parts entry");
                            SkipWhitespace();
                        }
                    }
                    if (model.empty()) return Fail("content.parts entry is missing model");
                    models.push_back(std::move(model));
                    if (models.size() > 128) return Fail("content.parts exceeds 128 entries");
                    SkipWhitespace();
                    if (Consume(']')) return true;
                    if (!Consume(',')) return Fail("expected ',' or ']' in content.parts");
                    SkipWhitespace();
                }
            }

            bool ParseContent(ManifestData& output)
            {
                if (!Consume('{')) return Fail("content must be an object");
                std::unordered_set<std::string> keys;
                SkipWhitespace();
                if (Consume('}')) return true;
                for (;;)
                {
                    std::string key;
                    if (!ParseString(key)) return false;
                    if (!keys.insert(key).second) return Fail("duplicate content key '" + key + "'");
                    SkipWhitespace();
                    if (!Consume(':')) return Fail("expected ':' in content");
                    SkipWhitespace();
                    if (key == "gameRoot")
                    {
                        if (!Mark(kGameRoot, "content.gameRoot") || !ParseString(output.gameRoot)) return false;
                    }
                    else if (key == "primary")
                    {
                        if (!ParseString(output.primary)) return false;
                    }
                    else if (key == "parts")
                    {
                        if (!Mark(kModels, "content.parts") || !ParseParts(output.models)) return false;
                    }
                    else if (key == "thumbnail")
                    {
                        if (!Mark(kThumbnail, "content.thumbnail") || !ParseString(output.thumbnail)) return false;
                    }
                    else if (!SkipValue(0)) return false;

                    SkipWhitespace();
                    if (Consume('}')) return true;
                    if (!Consume(',')) return Fail("expected ',' or '}' in content");
                    SkipWhitespace();
                }
            }

            bool ParseLicense(ManifestData& output)
            {
                if (!Consume('{')) return Fail("license must be an object");
                std::unordered_set<std::string> keys;
                SkipWhitespace();
                if (Consume('}')) return true;
                for (;;)
                {
                    std::string key;
                    if (!ParseString(key)) return false;
                    if (!keys.insert(key).second) return Fail("duplicate license key '" + key + "'");
                    SkipWhitespace();
                    if (!Consume(':')) return Fail("expected ':' in license");
                    SkipWhitespace();
                    if (key == "attributionFile")
                    {
                        if (!Mark(kAttributionFile, "license.attributionFile") ||
                            !ParseString(output.attributionFile)) return false;
                    }
                    else if (key == "attribution")
                    {
                        if (!Mark(kAttribution, "license.attribution") ||
                            !ParseString(output.attribution)) return false;
                    }
                    else if (!SkipValue(0)) return false;

                    SkipWhitespace();
                    if (Consume('}')) return true;
                    if (!Consume(',')) return Fail("expected ',' or '}' in license");
                    SkipWhitespace();
                }
            }

            void SkipWhitespace()
            {
                while (pos_ < source_.size() &&
                       std::isspace(static_cast<unsigned char>(source_[pos_])))
                    ++pos_;
            }

            bool Consume(char expected)
            {
                if (pos_ >= source_.size() || source_[pos_] != expected) return false;
                ++pos_;
                return true;
            }

            bool ParseString(std::string& output)
            {
                output.clear();
                if (!Consume('"')) return Fail("expected JSON string");
                while (pos_ < source_.size())
                {
                    const unsigned char ch = static_cast<unsigned char>(source_[pos_++]);
                    if (ch == '"') return true;
                    if (ch < 0x20) return Fail("unescaped control character in string");
                    if (ch != '\\')
                    {
                        output.push_back(static_cast<char>(ch));
                    }
                    else
                    {
                        if (pos_ >= source_.size()) return Fail("unfinished string escape");
                        const char escaped = source_[pos_++];
                        switch (escaped)
                        {
                            case '"': output.push_back('"'); break;
                            case '\\': output.push_back('\\'); break;
                            case '/': output.push_back('/'); break;
                            case 'b': output.push_back('\b'); break;
                            case 'f': output.push_back('\f'); break;
                            case 'n': output.push_back('\n'); break;
                            case 'r': output.push_back('\r'); break;
                            case 't': output.push_back('\t'); break;
                            case 'u':
                            {
                                uint32_t codepoint = 0;
                                if (!ParseHex4(codepoint)) return false;
                                if (codepoint >= 0xD800 && codepoint <= 0xDBFF)
                                {
                                    if (pos_ + 2 > source_.size() || source_[pos_] != '\\' ||
                                        source_[pos_ + 1] != 'u')
                                        return Fail("high surrogate is missing its low surrogate");
                                    pos_ += 2;
                                    uint32_t low = 0;
                                    if (!ParseHex4(low)) return false;
                                    if (low < 0xDC00 || low > 0xDFFF)
                                        return Fail("invalid low surrogate");
                                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) +
                                                (low - 0xDC00);
                                }
                                else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF)
                                    return Fail("unexpected low surrogate");
                                AppendUtf8(codepoint, output);
                                break;
                            }
                            default: return Fail("invalid string escape");
                        }
                    }
                    if (output.size() > 8192) return Fail("string exceeds 8192 bytes");
                }
                return Fail("unterminated string");
            }

            bool ParseHex4(uint32_t& value)
            {
                if (pos_ + 4 > source_.size()) return Fail("short unicode escape");
                value = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const unsigned char ch = static_cast<unsigned char>(source_[pos_++]);
                    value <<= 4;
                    if (ch >= '0' && ch <= '9') value |= ch - '0';
                    else if (ch >= 'a' && ch <= 'f') value |= ch - 'a' + 10;
                    else if (ch >= 'A' && ch <= 'F') value |= ch - 'A' + 10;
                    else return Fail("invalid unicode escape");
                }
                return true;
            }

            static void AppendUtf8(uint32_t codepoint, std::string& output)
            {
                if (codepoint <= 0x7F)
                    output.push_back(static_cast<char>(codepoint));
                else if (codepoint <= 0x7FF)
                {
                    output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
                else if (codepoint <= 0xFFFF)
                {
                    output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
                else
                {
                    output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                }
            }

            bool ParseNumber(double& output)
            {
                const size_t start = pos_;
                if (pos_ < source_.size() && source_[pos_] == '-') ++pos_;
                if (pos_ >= source_.size()) return Fail("unfinished number");
                if (source_[pos_] == '0')
                    ++pos_;
                else
                {
                    if (source_[pos_] < '1' || source_[pos_] > '9')
                        return Fail("invalid number");
                    while (pos_ < source_.size() && source_[pos_] >= '0' && source_[pos_] <= '9')
                        ++pos_;
                }
                if (pos_ < source_.size() && source_[pos_] == '.')
                {
                    ++pos_;
                    const size_t fraction = pos_;
                    while (pos_ < source_.size() && source_[pos_] >= '0' && source_[pos_] <= '9')
                        ++pos_;
                    if (fraction == pos_) return Fail("fraction has no digits");
                }
                if (pos_ < source_.size() && (source_[pos_] == 'e' || source_[pos_] == 'E'))
                {
                    ++pos_;
                    if (pos_ < source_.size() && (source_[pos_] == '+' || source_[pos_] == '-')) ++pos_;
                    const size_t exponent = pos_;
                    while (pos_ < source_.size() && source_[pos_] >= '0' && source_[pos_] <= '9')
                        ++pos_;
                    if (exponent == pos_) return Fail("exponent has no digits");
                }

                const std::string token = source_.substr(start, pos_ - start);
                char* end = nullptr;
                errno = 0;
                output = std::strtod(token.c_str(), &end);
                if (errno == ERANGE || !end || *end != '\0' || !std::isfinite(output))
                    return Fail("number is out of range");
                return true;
            }

            bool ParseStringList(std::vector<std::string>& output)
            {
                output.clear();
                if (pos_ < source_.size() && source_[pos_] == '"')
                {
                    std::string value;
                    if (!ParseString(value)) return false;
                    output.push_back(std::move(value));
                    return true;
                }
                if (!Consume('[')) return Fail("model/models must be a string or string array");
                SkipWhitespace();
                if (Consume(']')) return true;
                for (;;)
                {
                    std::string value;
                    if (!ParseString(value)) return false;
                    output.push_back(std::move(value));
                    if (output.size() > 128) return Fail("models array exceeds 128 entries");
                    SkipWhitespace();
                    if (Consume(']')) return true;
                    if (!Consume(',')) return Fail("expected ',' or ']' in models array");
                    SkipWhitespace();
                }
            }

            bool SkipValue(int depth)
            {
                if (depth > 32) return Fail("JSON nesting exceeds 32 levels");
                SkipWhitespace();
                if (pos_ >= source_.size()) return Fail("missing JSON value");
                if (source_[pos_] == '"')
                {
                    std::string ignored;
                    return ParseString(ignored);
                }
                if (Consume('{'))
                {
                    SkipWhitespace();
                    if (Consume('}')) return true;
                    for (;;)
                    {
                        std::string ignoredKey;
                        if (!ParseString(ignoredKey)) return false;
                        SkipWhitespace();
                        if (!Consume(':')) return Fail("expected ':' in object");
                        if (!SkipValue(depth + 1)) return false;
                        SkipWhitespace();
                        if (Consume('}')) return true;
                        if (!Consume(',')) return Fail("expected ',' or '}' in object");
                        SkipWhitespace();
                    }
                }
                if (Consume('['))
                {
                    SkipWhitespace();
                    if (Consume(']')) return true;
                    for (;;)
                    {
                        if (!SkipValue(depth + 1)) return false;
                        SkipWhitespace();
                        if (Consume(']')) return true;
                        if (!Consume(',')) return Fail("expected ',' or ']' in array");
                        SkipWhitespace();
                    }
                }
                for (const char* literal : { "true", "false", "null" })
                {
                    const size_t length = std::strlen(literal);
                    if (source_.compare(pos_, length, literal) == 0)
                    {
                        pos_ += length;
                        return true;
                    }
                }
                double ignored = 0.0;
                return ParseNumber(ignored);
            }

            bool Fail(const std::string& message)
            {
                if (error_.empty())
                {
                    std::ostringstream stream;
                    stream << message << " at byte " << pos_;
                    error_ = stream.str();
                }
                return false;
            }

            bool Finish(std::string& output, const std::string& message)
            {
                Fail(message);
                output = error_;
                return false;
            }

            bool CopyError(std::string& output)
            {
                output = error_.empty() ? "invalid JSON" : error_;
                return false;
            }

            const std::string& source_;
            size_t pos_ = 0;
            std::string error_;
            bool seen_[kFieldCount] = {};
        };

        bool ReadManifestFile(const std::string& path, std::string& contents, std::string& error)
        {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input)
            {
                error = "cannot open manifest";
                return false;
            }
            const std::streamoff length = input.tellg();
            if (length <= 0)
            {
                error = "manifest is empty";
                return false;
            }
            constexpr std::streamoff kMaxManifestBytes = 128 * 1024;
            if (length > kMaxManifestBytes)
            {
                error = "manifest exceeds 128 KiB";
                return false;
            }
            contents.resize(static_cast<size_t>(length));
            input.seekg(0, std::ios::beg);
            input.read(&contents[0], length);
            if (!input)
            {
                error = "failed while reading manifest";
                return false;
            }
            return true;
        }

        bool HasControlCharacters(const std::string& value, bool allowWhitespace)
        {
            return std::any_of(value.begin(), value.end(), [allowWhitespace](char raw) {
                const unsigned char ch = static_cast<unsigned char>(raw);
                return ch < 0x20 && (!allowWhitespace || (ch != '\t' && ch != '\r' && ch != '\n'));
            });
        }

        bool IsPackageId(const std::string& value)
        {
            if (value.empty() || value.front() == '_' || value.back() == '_') return false;
            bool previousUnderscore = false;
            for (char raw : value)
            {
                const unsigned char ch = static_cast<unsigned char>(raw);
                const bool underscore = ch == '_';
                if (!underscore && !(ch >= 'a' && ch <= 'z') && !(ch >= '0' && ch <= '9'))
                    return false;
                if (underscore && previousUnderscore) return false;
                previousUnderscore = underscore;
            }
            return true;
        }

        bool StartsWithNoCase(const std::string& value, const char* prefix)
        {
            const size_t length = std::strlen(prefix);
            return value.size() >= length && _strnicmp(value.c_str(), prefix, length) == 0;
        }

        enum ManifestPathKind
        {
            kManifestModel,
            kManifestThumbnail,
            kManifestAttribution,
        };

        bool NormalizeManifestPath(const std::string& input, const std::string& directory,
                                   ManifestPathKind kind, std::string& output, std::string& error)
        {
            if (input.empty())
            {
                error = kind == kManifestModel ? "model path is empty" : "asset path is empty";
                return false;
            }
            if (input.size() >= 480)
            {
                error = "asset path exceeds 479 bytes";
                return false;
            }

            std::string normalized = input;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            if (normalized.front() == '/' || normalized.find(':') != std::string::npos ||
                normalized.find("..") != std::string::npos ||
                StartsWithNoCase(normalized, "Data/") || StartsWithNoCase(normalized, "Patch-9.MPQ/"))
            {
                error = "path must be archive-relative, not absolute or Data/Patch-9.MPQ-prefixed";
                return false;
            }

            if (!StartsWithNoCase(normalized, "World/"))
                normalized = "World/wxl_housing/custom/" + directory + "/" + normalized;

            size_t offset = 0;
            while (offset <= normalized.size())
            {
                const size_t slash = normalized.find('/', offset);
                const size_t end = slash == std::string::npos ? normalized.size() : slash;
                const std::string segment = normalized.substr(offset, end - offset);
                if (segment.empty() || segment == "." || segment == "..")
                {
                    error = "path contains an empty, '.' or '..' segment";
                    return false;
                }
                if (segment.back() == '.' || segment.back() == ' ' ||
                    std::any_of(segment.begin(), segment.end(), [](char raw) {
                        const unsigned char ch = static_cast<unsigned char>(raw);
                        return ch < 0x20 || ch == '"' || ch == '*' || ch == '?' ||
                               ch == '<' || ch == '>' || ch == '|';
                    }))
                {
                    error = "path contains characters unsafe on Windows/client storage";
                    return false;
                }
                if (slash == std::string::npos) break;
                offset = slash + 1;
            }

            constexpr char kCustomPrefix[] = "World/wxl_housing/custom/";
            if (!StartsWithNoCase(normalized, kCustomPrefix) ||
                normalized.size() <= sizeof(kCustomPrefix) - 1)
            {
                error = "asset path must stay under World/wxl_housing/custom";
                return false;
            }

            if (kind == kManifestModel)
            {
                if (!EndsWithNoCase(normalized, ".m2"))
                {
                    error = "model path must end in .m2";
                    return false;
                }
            }
            else if (kind == kManifestThumbnail &&
                     !(EndsWithNoCase(normalized, ".png") || EndsWithNoCase(normalized, ".jpg") ||
                       EndsWithNoCase(normalized, ".jpeg") || EndsWithNoCase(normalized, ".bmp") ||
                       EndsWithNoCase(normalized, ".dds") || EndsWithNoCase(normalized, ".tga")))
            {
                error = "thumbnail must be PNG, JPG, BMP, DDS or TGA";
                return false;
            }
            else if (kind == kManifestAttribution &&
                     !(EndsWithNoCase(normalized, ".txt") || EndsWithNoCase(normalized, ".md")))
            {
                error = "attribution file must be TXT or Markdown";
                return false;
            }

            std::replace(normalized.begin(), normalized.end(), '/', '\\');
            output = std::move(normalized);
            return true;
        }

        size_t LoadCustomManifests(std::vector<DecorRow>& rows, std::vector<std::string>& errors)
        {
            rows.erase(std::remove_if(rows.begin(), rows.end(), [](const DecorRow& row) {
                return row.discovered;
            }), rows.end());
            errors.clear();

            std::unordered_set<uint32_t> usedRowIds;
            usedRowIds.reserve(rows.size() + 32);
            for (const DecorRow& row : rows) usedRowIds.insert(row.rowId);

            constexpr char kRoot[] = "Data\\Patch-9.MPQ\\World\\wxl_housing\\custom";
            WIN32_FIND_DATAA entry = {};
            HANDLE search = FindFirstFileA("Data\\Patch-9.MPQ\\World\\wxl_housing\\custom\\*", &entry);
            std::vector<std::string> directories;
            if (search != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                        std::strcmp(entry.cFileName, ".") == 0 || std::strcmp(entry.cFileName, "..") == 0)
                        continue;
                    if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    {
                        const std::string message = std::string(entry.cFileName) +
                            "\\decor.json: reparse-point directory rejected";
                        errors.push_back(message);
                        WLOG_WARN("catalog: custom manifest %s", message.c_str());
                        continue;
                    }
                    directories.emplace_back(entry.cFileName);
                } while (FindNextFileA(search, &entry));
                FindClose(search);
            }

            std::sort(directories.begin(), directories.end(), [](const std::string& a, const std::string& b) {
                return _stricmp(a.c_str(), b.c_str()) < 0;
            });

            size_t accepted = 0;
            for (const std::string& directory : directories)
            {
                const std::string relative = directory + "\\decor.json";
                const std::string path = std::string(kRoot) + "\\" + relative;
                const DWORD attributes = GetFileAttributesA(path.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES) continue;

                auto reject = [&](const std::string& reason) {
                    const std::string message = relative + ": " + reason;
                    errors.push_back(message);
                    WLOG_WARN("catalog: custom manifest %s", message.c_str());
                };

                if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                {
                    reject("decor.json is not a regular non-reparse file");
                    continue;
                }

                std::string contents;
                std::string parseError;
                if (!ReadManifestFile(path, contents, parseError))
                {
                    reject(parseError);
                    continue;
                }

                ManifestData manifest;
                ManifestParser parser(contents);
                if (!parser.Parse(manifest, parseError))
                {
                    reject(parseError);
                    continue;
                }
                const bool richManifest = !manifest.schema.empty();
                if (richManifest && (!IsPackageId(manifest.packageId) ||
                    _stricmp(manifest.packageId.c_str(), directory.c_str()) != 0))
                {
                    reject("package id must match its custom directory name");
                    continue;
                }
                if (richManifest && manifest.kind != "single-m2" && manifest.kind != "composite-m2")
                {
                    reject("catalog.kind must be single-m2 or composite-m2");
                    continue;
                }
                if (richManifest)
                {
                    std::string normalizedGameRoot = manifest.gameRoot;
                    std::replace(normalizedGameRoot.begin(), normalizedGameRoot.end(), '/', '\\');
                    const std::string expectedGameRoot =
                        "World\\wxl_housing\\custom\\" + directory;
                    if (_stricmp(normalizedGameRoot.c_str(), expectedGameRoot.c_str()) != 0)
                    {
                        reject("content.gameRoot must exactly match " + expectedGameRoot);
                        continue;
                    }
                }
                if (manifest.name.empty() || manifest.name.size() > 128 ||
                    HasControlCharacters(manifest.name, false))
                {
                    reject("name must contain 1-128 printable bytes");
                    continue;
                }
                if (manifest.category.empty() || manifest.category.size() > 64 ||
                    HasControlCharacters(manifest.category, false))
                {
                    reject("category must contain 1-64 printable bytes");
                    continue;
                }
                if (manifest.models.empty())
                {
                    reject("models must contain at least one M2 path");
                    continue;
                }
                if (richManifest && ((manifest.kind == "single-m2" && manifest.models.size() != 1) ||
                                     (manifest.kind == "composite-m2" && manifest.models.size() < 2)))
                {
                    reject("catalog.kind does not match the number of content.parts entries");
                    continue;
                }
                if (!std::isfinite(manifest.scale) || manifest.scale < 1.0f / 1024.0f ||
                    manifest.scale > 65535.0f / 1024.0f)
                {
                    reject("scale must be between 1/1024 and 65535/1024");
                    continue;
                }
                if (!std::isfinite(manifest.spawnDistance) || manifest.spawnDistance < 1.0f ||
                    manifest.spawnDistance > 60.0f)
                {
                    reject("spawn_distance must be between 1 and 60");
                    continue;
                }
                if (manifest.attribution.size() > 1024 ||
                    HasControlCharacters(manifest.attribution, true))
                {
                    reject("attribution exceeds 1024 bytes or contains unsafe controls");
                    continue;
                }
                if (usedRowIds.find(manifest.rowId) != usedRowIds.end())
                {
                    std::ostringstream reason;
                    reason << "duplicate row id " << manifest.rowId;
                    reject(reason.str());
                    continue;
                }

                std::vector<std::string> normalizedModels;
                normalizedModels.reserve(manifest.models.size());
                std::unordered_set<std::string> uniqueModels;
                bool modelsValid = true;
                for (const std::string& model : manifest.models)
                {
                    const std::string modelInput = manifest.gameRoot.empty()
                        ? model : manifest.gameRoot + "\\" + model;
                    std::string normalized;
                    std::string reason;
                    if (!NormalizeManifestPath(modelInput, directory, kManifestModel, normalized, reason))
                    {
                        reject(reason + ": " + model);
                        modelsValid = false;
                        break;
                    }
                    std::string folded = normalized;
                    std::transform(folded.begin(), folded.end(), folded.begin(), [](char ch) {
                        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    });
                    if (!uniqueModels.insert(folded).second)
                    {
                        reject("duplicate model path: " + model);
                        modelsValid = false;
                        break;
                    }
                    normalizedModels.push_back(std::move(normalized));
                }
                if (!modelsValid) continue;

                if (richManifest)
                {
                    if (manifest.primary.empty())
                    {
                        reject("content.primary is required");
                        continue;
                    }
                    std::string normalizedPrimary;
                    std::string reason;
                    if (!NormalizeManifestPath(manifest.gameRoot + "\\" + manifest.primary,
                                               directory, kManifestModel, normalizedPrimary, reason))
                    {
                        reject(reason + ": " + manifest.primary);
                        continue;
                    }
                    if (_stricmp(normalizedPrimary.c_str(), normalizedModels.front().c_str()) != 0)
                    {
                        reject("content.primary must equal the first content.parts model");
                        continue;
                    }
                }

                std::string normalizedThumbnail;
                if (!manifest.thumbnail.empty())
                {
                    const std::string thumbnailInput = manifest.gameRoot.empty()
                        ? manifest.thumbnail : manifest.gameRoot + "\\" + manifest.thumbnail;
                    std::string reason;
                    if (!NormalizeManifestPath(thumbnailInput, directory, kManifestThumbnail,
                                               normalizedThumbnail, reason))
                    {
                        reject(reason + ": " + manifest.thumbnail);
                        continue;
                    }
                }

                if (!manifest.attributionFile.empty())
                {
                    const std::string attributionInput = manifest.gameRoot.empty()
                        ? manifest.attributionFile : manifest.gameRoot + "\\" + manifest.attributionFile;
                    std::string normalizedAttribution;
                    std::string reason;
                    if (!NormalizeManifestPath(attributionInput, directory, kManifestAttribution,
                                               normalizedAttribution, reason))
                    {
                        reject(reason + ": " + manifest.attributionFile);
                        continue;
                    }
                    std::string diskAttribution = "Data\\Patch-9.MPQ\\" + normalizedAttribution;
                    std::string attributionContents;
                    if (!ReadManifestFile(diskAttribution, attributionContents, reason))
                    {
                        reject("license.attributionFile " + reason);
                        continue;
                    }
                    if (manifest.attribution.empty())
                    {
                        if (attributionContents.size() >= 3 &&
                            static_cast<unsigned char>(attributionContents[0]) == 0xEF &&
                            static_cast<unsigned char>(attributionContents[1]) == 0xBB &&
                            static_cast<unsigned char>(attributionContents[2]) == 0xBF)
                            attributionContents.erase(0, 3);
                        while (!attributionContents.empty() &&
                               std::isspace(static_cast<unsigned char>(attributionContents.back())))
                            attributionContents.pop_back();
                        if (attributionContents.size() > 8192 ||
                            HasControlCharacters(attributionContents, true))
                        {
                            reject("license.attributionFile exceeds 8192 bytes or contains unsafe controls");
                            continue;
                        }
                        manifest.attribution = std::move(attributionContents);
                    }
                }

                DecorRow row = {};
                row.rowId = manifest.rowId;
                row.initialScale = manifest.scale;
                row.type = 1;      // custom manifest props are free-standing M2s
                row.modelType = 1; // M2
                row.orderIndex = manifest.rowId;
                row.name = std::move(manifest.name);
                row.modelParts = std::move(normalizedModels);
                row.modelPath = row.modelParts.front();
                row.thumbPath = std::move(normalizedThumbnail);
                row.customCategory = std::move(manifest.category);
                row.attribution = std::move(manifest.attribution);
                row.spawnDistance = manifest.spawnDistance;
                row.dnt = IsDntName(row.name);
                row.custom = true;
                row.discovered = true;
                row.assetInstalled = LocalModelsExist(row);
                row.placeable = IsPlaceableM2(row);
                WLOG_INFO("catalog: custom manifest accepted %s row=%u parts=%zu primary=%s installed=%d placeable=%d",
                          relative.c_str(), row.rowId, row.modelParts.size(), row.modelPath.c_str(),
                          row.assetInstalled ? 1 : 0, row.placeable ? 1 : 0);
                usedRowIds.insert(row.rowId);
                rows.push_back(std::move(row));
                ++accepted;
            }

            return accepted;
        }

        void DrawPreviewPlaceholder(const char* label, const ImVec2& size)
        {
            const ImVec2 topLeft = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##preview", size);
            const ImVec2 bottomRight(topLeft.x + size.x, topLeft.y + size.y);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(topLeft, bottomRight, IM_COL32(31, 36, 43, 255), 5.0f);
            draw->AddRect(topLeft, bottomRight, IM_COL32(73, 82, 94, 255), 5.0f);
            const ImVec2 textSize = ImGui::CalcTextSize(label);
            draw->AddText(ImVec2(topLeft.x + (size.x - textSize.x) * 0.5f,
                                 topLeft.y + (size.y - textSize.y) * 0.5f),
                          IM_COL32(145, 153, 164, 255), label);
        }

        std::unordered_map<uint32_t, std::string> LoadRetailNames()
        {
            std::ifstream in;
            for (const char* path : {
                    "DBFilesClient\\HouseDecorNames.tsv",
                    "Data\\Patch-Z.MPQ\\DBFilesClient\\HouseDecorNames.tsv" })
            {
                in.open(path, std::ios::binary);
                if (in) break;
                in.clear();
            }

            std::unordered_map<uint32_t, std::string> names;
            if (!in) return names;
            names.reserve(3000);
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t tab = line.find('\t');
                if (tab == std::string::npos) continue;
                const uint32_t id = static_cast<uint32_t>(std::strtoul(line.c_str(), nullptr, 10));
                if (id) names.emplace(id, line.substr(tab + 1));
            }
            return names;
        }

        void* TryLoad(const WXL_Db2Field* fields, uint32_t fieldCount, uint32_t layoutHash,
                      char* error, size_t errorSize)
        {
            WXL_Db2Definition def = {};
            def.name          = "HouseDecor";
            def.filename      = "housedecor.db2";
            def.layoutHash    = layoutHash;
            def.fields        = fields;
            def.fieldCount    = fieldCount;
            def.relations     = nullptr;
            def.relationCount = 0;
            def.required      = 0;
            return g_db2->Load(&def, error, errorSize);
        }
    }

    Catalog& Catalog::Instance()
    {
        static Catalog s;
        return s;
    }

    bool Catalog::EnsureLoaded()
    {
        if (loaded_) return true;
        if (!g_db2 || !g_fdid) return false;

        char err121[256] = {};
        char err1207[256] = {};
        table_ = TryLoad(kFields121, kFieldCount121, kLayout121, err121, sizeof err121);
        uint32_t activeLayout = kLayout121;
        if (!table_)
        {
            table_ = TryLoad(kFields1207, kFieldCount1207, kLayout1207, err1207, sizeof err1207);
            activeLayout = kLayout1207;
        }
        if (!table_)
        {
            WLOG_ERROR("catalog: HouseDecor load failed (12.1: %s; 12.0.7: %s)",
                       err121[0] ? err121 : "unknown", err1207[0] ? err1207 : "unknown");
            return false;
        }

        const auto retailNames = LoadRetailNames();
        const uint32_t n = g_db2->RowCount(table_);
        rows_.clear();
        rows_.reserve(static_cast<size_t>(n) + 32u);
        for (uint32_t i = 0; i < n; ++i)
        {
            const void* row = g_db2->RowAt(table_, i);
            if (!row) continue;

            DecorRow r;
            r.rowId         = g_db2->Value(table_, row, "ID", 0);
            if (!r.rowId) r.rowId = g_db2->RowId(row);
            if (!r.rowId) continue; // encrypted/all-zero physical record
            r.modelFdid     = g_db2->Value(table_, row, "ModelFileDataID", 0);
            r.thumbFdid     = g_db2->Value(table_, row, "ThumbnailFileDataID", 0);
            r.type          = g_db2->Value(table_, row, "Type", 0);
            r.modelType     = g_db2->Value(table_, row, "ModelType", 0);
            r.itemId        = g_db2->Value(table_, row, "ItemID", 0);
            r.flags         = g_db2->Value(table_, row, "Flags", 0);
            r.orderIndex    = g_db2->Value(table_, row, "OrderIndex", 0);
            // InitialScale is a float; Value() returns the raw cell bits.
            const uint32_t scaleBits = g_db2->Value(table_, row, "InitialScale", 0);
            std::memcpy(&r.initialScale, &scaleBits, sizeof r.initialScale);

            if (const auto it = retailNames.find(r.rowId); it != retailNames.end())
                r.name = it->second;

            // wxl.fdid hands back a backslash path; normalize for display.
            if (r.modelFdid)
            if (const char* p = g_fdid->ResolveModel(r.modelFdid))
            {
                r.modelPath = p;
                std::replace(r.modelPath.begin(), r.modelPath.end(), '\\', '/');
            }
            if (r.thumbFdid)
            if (const char* p = g_fdid->ResolveTexture(r.thumbFdid))
            {
                r.thumbPath = p;
                std::replace(r.thumbPath.begin(), r.thumbPath.end(), '\\', '/');
            }

            if (r.name.empty())
            {
                if (!r.modelPath.empty())
                {
                    const char* base = std::strrchr(r.modelPath.c_str(), '/');
                    r.name = base ? base + 1 : r.modelPath;
                }
                else
                {
                    char fallback[40];
                    std::snprintf(fallback, sizeof fallback, "Decor %u", r.rowId);
                    r.name = fallback;
                }
            }
            r.dnt = IsDntName(r.name);
            r.assetInstalled = LocalModelsExist(r);
            r.placeable = IsPlaceableM2(r);

            rows_.push_back(r);
        }
        customManifestCount_ = LoadCustomManifests(rows_, customScanErrors_);
        loaded_ = true;
        RebuildFiltered();
        WLOG_INFO("catalog: HouseDecor layout=%08X physical=%u catalog=%zu names=%zu paths=%zu placeable=%zu custom=%zu errors=%zu",
                  activeLayout, n, rows_.size(), retailNames.size(),
                  std::count_if(rows_.begin(), rows_.end(),
                                [](const DecorRow& r) { return !r.modelPath.empty(); }),
                  std::count_if(rows_.begin(), rows_.end(),
                                [](const DecorRow& r) { return r.placeable; }),
                  customManifestCount_, customScanErrors_.size());
        return true;
    }

    const DecorRow* Catalog::Find(uint32_t rowId) const
    {
        for (const auto& r : rows_)
            if (r.rowId == rowId) return &r;
        return nullptr;
    }

    void Catalog::RebuildFiltered()
    {
        filtered_.clear();
        const char* q = filter_;
        for (const auto& r : rows_)
        {
            if (!includeDnt_ && r.dnt) continue;
            if (placeableOnly_ && !r.placeable) continue;
            if (categoryFilter_ != kCategoryAll && CategoryFor(r) != categoryFilter_) continue;
            if (typeFilter_ >= 0 && r.type != static_cast<uint32_t>(typeFilter_)) continue;
            if (q[0] == '\0') { filtered_.push_back(&r); continue; }
            char idbuf[16];
            snprintf(idbuf, sizeof idbuf, "%u", r.rowId);
            if (std::strstr(idbuf, q) || ContainsNoCase(r.name, q) ||
                (!r.modelPath.empty() && ContainsNoCase(r.modelPath, q)) ||
                (!r.customCategory.empty() && ContainsNoCase(r.customCategory, q)) ||
                (!r.attribution.empty() && ContainsNoCase(r.attribution, q)) ||
                std::any_of(r.modelParts.begin(), r.modelParts.end(),
                    [q](const std::string& part) { return ContainsNoCase(part, q); }))
                filtered_.push_back(&r);
        }
        if (!filtered_.empty() && !Find(selectedRow_)) selectedRow_ = filtered_.front()->rowId;
        if (!filtered_.empty() && std::none_of(filtered_.begin(), filtered_.end(),
                [this](const DecorRow* row) { return row->rowId == selectedRow_; }))
            selectedRow_ = filtered_.front()->rowId;
    }

    void Catalog::RescanCustomProps()
    {
        for (auto& pair : customThumbnails_)
            if (pair.second) reinterpret_cast<IDirect3DTexture9*>(pair.second)->Release();
        customThumbnails_.clear();
        customManifestCount_ = LoadCustomManifests(rows_, customScanErrors_);
        RebuildFiltered();
        WLOG_INFO("catalog: custom rescan accepted=%zu errors=%zu catalog=%zu",
                  customManifestCount_, customScanErrors_.size(), rows_.size());
    }

    void Catalog::Place(const DecorRow& row)
    {
        if (Placement::Instance().SpawnRow(row))
        {
            selectedRow_ = row.rowId;
            ++placeCount_;
            WLOG_INFO("catalog: place row=%u fdid=%u -> %s",
                      row.rowId, row.modelFdid, row.modelPath.c_str());
        }
        else
            WLOG_WARN("catalog: place failed row=%u (model not resolvable?)", row.rowId);
    }

    void* Catalog::LoadTextureFile(uint32_t cacheKey, const char* path)
    {
        if (!device_ || !path || !*path) return nullptr;
        const auto cached = thumbnails_.find(cacheKey);
        if (cached != thumbnails_.end()) return cached->second;
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        {
            thumbnails_.emplace(cacheKey, nullptr);
            return nullptr;
        }

        using CreateTextureFn = HRESULT (WINAPI*)(IDirect3DDevice9*, LPCSTR, IDirect3DTexture9**);
        static CreateTextureFn createTexture = []() -> CreateTextureFn {
            HMODULE module = GetModuleHandleA("d3dx9_43.dll");
            if (!module) module = LoadLibraryA("d3dx9_43.dll");
            return module ? reinterpret_cast<CreateTextureFn>(
                GetProcAddress(module, "D3DXCreateTextureFromFileA")) : nullptr;
        }();

        IDirect3DTexture9* texture = nullptr;
        if (!createTexture || FAILED(createTexture(
                reinterpret_cast<IDirect3DDevice9*>(device_), path, &texture)))
            texture = nullptr;
        thumbnails_.emplace(cacheKey, texture);
        return texture;
    }

    void* Catalog::LauncherTexture(void* device)
    {
        device_ = device;
        return LoadTextureFile(kUiTextureCategoryNavigation,
            "Extensions\\wxl-housing\\ui\\housingitemcategorynavigation.png");
    }

    void* Catalog::LoadThumbnail(uint32_t fdid)
    {
        if (!fdid || !device_) return nullptr;
        const auto cached = thumbnails_.find(fdid);
        if (cached != thumbnails_.end()) return cached->second;
        if (thumbnailLoadsRemaining_ <= 0) return nullptr;
        --thumbnailLoadsRemaining_;

        char path[MAX_PATH];
        std::snprintf(path, sizeof path, "Extensions\\wxl-housing\\thumbnails\\%u.png", fdid);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        {
            thumbnails_.emplace(fdid, nullptr); // remember unavailable previews
            return nullptr;
        }

        return LoadTextureFile(fdid, path);
    }

    void* Catalog::LoadRowThumbnail(const DecorRow& row)
    {
        if (!row.custom || row.thumbPath.empty()) return LoadThumbnail(row.thumbFdid);
        if (!device_) return nullptr;

        const auto cached = customThumbnails_.find(row.thumbPath);
        if (cached != customThumbnails_.end()) return cached->second;
        if (thumbnailLoadsRemaining_ <= 0) return nullptr;
        --thumbnailLoadsRemaining_;

        std::string diskPath = "Data\\Patch-9.MPQ\\" + row.thumbPath;
        std::replace(diskPath.begin(), diskPath.end(), '/', '\\');
        if (GetFileAttributesA(diskPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            customThumbnails_.emplace(row.thumbPath, nullptr);
            return nullptr;
        }

        using CreateTextureFn = HRESULT (WINAPI*)(IDirect3DDevice9*, LPCSTR, IDirect3DTexture9**);
        static CreateTextureFn createTexture = []() -> CreateTextureFn {
            HMODULE module = GetModuleHandleA("d3dx9_43.dll");
            if (!module) module = LoadLibraryA("d3dx9_43.dll");
            return module ? reinterpret_cast<CreateTextureFn>(
                GetProcAddress(module, "D3DXCreateTextureFromFileA")) : nullptr;
        }();

        IDirect3DTexture9* texture = nullptr;
        if (!createTexture || FAILED(createTexture(
                reinterpret_cast<IDirect3DDevice9*>(device_), diskPath.c_str(), &texture)))
            texture = nullptr;
        customThumbnails_.emplace(row.thumbPath, texture);
        return texture;
    }

    void Catalog::DrawCard(const DecorRow& row, float size)
    {
        ImGui::PushID(static_cast<int>(row.rowId));
        const ImVec2 topLeft = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##decor-card", ImVec2(size, size));
        if (ImGui::IsItemClicked()) selectedRow_ = row.rowId;

        const bool selected = selectedRow_ == row.rowId;
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 bottomRight(topLeft.x + size, topLeft.y + size);
        draw->AddRectFilled(topLeft, bottomRight,
            hovered ? IM_COL32(32, 28, 22, 250) : IM_COL32(13, 13, 14, 245), 7.0f);
        draw->AddRect(topLeft, bottomRight,
            selected ? IM_COL32(228, 188, 72, 255) : IM_COL32(118, 103, 76, 255),
            7.0f, 0, selected ? 2.5f : 1.2f);

        const float inset = 7.0f;
        const ImVec2 imageMin(topLeft.x + inset, topLeft.y + inset);
        const ImVec2 imageMax(bottomRight.x - inset, bottomRight.y - inset);
        if (void* texture = LoadRowThumbnail(row))
            draw->AddImage(reinterpret_cast<ImTextureID>(texture), imageMin, imageMax);
        else
        {
            const char* label = (row.thumbFdid || !row.thumbPath.empty()) ? "No preview" : "No image";
            const ImVec2 textSize = ImGui::CalcTextSize(label);
            draw->AddText(ImVec2(topLeft.x + (size - textSize.x) * 0.5f,
                                 topLeft.y + (size - textSize.y) * 0.5f),
                          IM_COL32(145, 153, 164, 255), label);
        }

        const ImU32 status = row.placeable ? IM_COL32(83, 190, 102, 255)
            : (row.modelType == 2 ? IM_COL32(214, 168, 66, 255) : IM_COL32(125, 125, 125, 255));
        draw->AddCircleFilled(ImVec2(bottomRight.x - 10.0f, bottomRight.y - 10.0f), 4.0f, status);
        if (hovered)
            ImGui::SetTooltip("%s\n%s\n%s", row.name.c_str(), TypeName(row.type), PlacementStatus(row));
        ImGui::PopID();
    }

    void Catalog::DrawCategoryRail(float height)
    {
        if (!ImGui::BeginChild("##category-rail", ImVec2(72.0f, height), true,
                               ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::EndChild();
            return;
        }
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("TYPE").x) * 0.5f);
        ImGui::TextDisabled("TYPE");
        ImGui::Separator();
        static const char* shortNames[] = {
            "ALL", "FURN", "ACCENT", "STRUCT", "LIGHT", "NATURE", "UTIL", "CUSTOM", "MISC"
        };
        struct IconCell { int activeX, activeY, inactiveX, inactiveY; };
        static const IconCell iconCells[] = {
            { 0, 0, 1, 0 },   // all / storage
            { 3, 3, 4, 3 },   // furnishings / chair
            { 12, 2, 13, 2 }, // accents / vase
            { 9, 4, 10, 4 },  // structural / brick wall
            { 9, 3, 10, 3 },  // lighting / candle
            { 9, 1, 10, 1 },  // nature / plant
            { 3, 2, 4, 2 },   // functional / materials
            { 0, 0, 1, 0 },   // custom props / storage
            { 9, 2, 10, 2 },  // misc / ellipsis
        };
        void* iconTexture = LoadTextureFile(kUiTextureCategoryNavigation,
            "Extensions\\wxl-housing\\ui\\housingitemcategorynavigation.png");
        for (int category = kCategoryAll; category <= kCategoryMisc; ++category)
        {
            ImGui::PushID(category);
            const bool selected = categoryFilter_ == category;
            bool clicked = false;
            if (iconTexture)
            {
                constexpr float cell = 64.0f;
                constexpr float stride = 66.0f;
                constexpr float atlas = 1024.0f;
                const IconCell& icon = iconCells[category];
                const int cellX = selected ? icon.activeX : icon.inactiveX;
                const int cellY = selected ? icon.activeY : icon.inactiveY;
                const ImVec2 topLeft = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##retail-category", ImVec2(50.0f, 50.0f));
                clicked = ImGui::IsItemClicked();
                const bool hovered = ImGui::IsItemHovered();
                const ImVec2 imageMin(topLeft.x + 1.0f, topLeft.y + 1.0f);
                const ImVec2 imageMax(topLeft.x + 49.0f, topLeft.y + 49.0f);
                const ImVec2 uv0(cellX * stride / atlas, cellY * stride / atlas);
                const ImVec2 uv1((cellX * stride + cell) / atlas,
                                 (cellY * stride + cell) / atlas);
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddImage(reinterpret_cast<ImTextureID>(iconTexture), imageMin, imageMax, uv0, uv1,
                    hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(235, 235, 235, 255));
                if (selected || hovered)
                    draw->AddRect(topLeft, ImVec2(topLeft.x + 50.0f, topLeft.y + 50.0f),
                        selected ? IM_COL32(244, 190, 55, 255) : IM_COL32(186, 143, 55, 220),
                        24.0f, 0, selected ? 2.0f : 1.0f);
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button,
                    selected ? ImVec4(0.42f, 0.31f, 0.10f, 1.0f) : ImVec4(0.10f, 0.09f, 0.08f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.52f, 0.39f, 0.14f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18.0f);
                clicked = ImGui::Button(shortNames[category], ImVec2(-1.0f, 38.0f));
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
            }
            if (clicked)
            {
                categoryFilter_ = category;
                RebuildFiltered();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", CategoryName(category));
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    void Catalog::DrawGrid(float height)
    {
        if (!ImGui::BeginChild("##catalog-grid", ImVec2(490.0f, height), true))
        {
            ImGui::EndChild();
            return;
        }
        ImGui::Text("%s", CategoryName(categoryFilter_));
        ImGui::SameLine();
        ImGui::TextDisabled("%zu items", filtered_.size());
        ImGui::Separator();

        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float available = ImGui::GetContentRegionAvail().x;
        const int columns = 3;
        const float cardSize = (available - spacing * (columns - 1)) / columns;
        const int visualRows = (static_cast<int>(filtered_.size()) + columns - 1) / columns;
        ImGuiListClipper clipper;
        clipper.Begin(visualRows, cardSize + ImGui::GetStyle().ItemSpacing.y);
        while (clipper.Step())
        for (int visualRow = clipper.DisplayStart; visualRow < clipper.DisplayEnd; ++visualRow)
        {
            for (int column = 0; column < columns; ++column)
            {
                const size_t index = static_cast<size_t>(visualRow * columns + column);
                if (index >= filtered_.size()) break;
                if (column > 0) ImGui::SameLine();
                DrawCard(*filtered_[index], cardSize);
            }
        }
        clipper.End();
        ImGui::EndChild();
    }

    void Catalog::DrawDetails(float height)
    {
        if (!ImGui::BeginChild("##catalog-details", ImVec2(0.0f, height), true))
        {
            ImGui::EndChild();
            return;
        }
        const DecorRow* row = Find(selectedRow_);
        if (!row)
        {
            ImGui::TextDisabled("Select an item to preview it.");
            ImGui::EndChild();
            return;
        }

        ImGui::TextColored(ImVec4(0.94f, 0.80f, 0.32f, 1.0f), "DECOR PREVIEW");
        ImGui::Separator();
        ImGui::TextWrapped("%s", row->name.c_str());
        ImGui::Spacing();
        ImGui::TextColored(row->placeable ? ImVec4(0.40f, 0.86f, 0.48f, 1.0f)
                                           : ImVec4(0.86f, 0.68f, 0.30f, 1.0f),
                           "%s", PlacementStatus(*row));
        ImGui::TextDisabled("%s  |  HouseDecor %u", TypeName(row->type), row->rowId);

        ImGui::Spacing();
        const float preview = std::min(286.0f, ImGui::GetContentRegionAvail().x);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - preview) * 0.5f);
        if (void* texture = LoadRowThumbnail(*row))
            ImGui::Image(reinterpret_cast<ImTextureID>(texture), ImVec2(preview, preview));
        else
            DrawPreviewPlaceholder((row->thumbFdid || !row->thumbPath.empty())
                                       ? "Preview unavailable" : "No preview",
                                   ImVec2(preview, preview));

        if (!row->modelPath.empty())
        {
            if (row->custom)
                ImGui::TextDisabled("Custom composite  |  %zu M2 parts", row->modelParts.size());
            else
                ImGui::TextDisabled("Model FDID %u", row->modelFdid);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row->modelPath.c_str());
        }
        if (row->custom)
        {
            ImGui::TextDisabled("Category: %s", row->customCategory.empty()
                ? "Custom Props" : row->customCategory.c_str());
            if (!row->attribution.empty()) ImGui::TextWrapped("%s", row->attribution.c_str());
        }
        ImGui::SetCursorPosY(height - 46.0f);
        ImGui::BeginDisabled(!row->placeable);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.48f, 0.08f, 0.04f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.13f, 0.06f, 1.0f));
        if (ImGui::Button("PLACE DECOR", ImVec2(-1.0f, 32.0f))) Place(*row);
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        if (!row->placeable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", PlacementStatus(*row));
        ImGui::EndChild();
    }

    void Catalog::DrawCompactTable(float height)
    {
        if (!ImGui::BeginTable("decor", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                ImVec2(0, height))) return;

        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 58.0f);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 58.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(filtered_.size()));
        while (clipper.Step())
        for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
        {
            const DecorRow& row = *filtered_[static_cast<size_t>(rowIndex)];
            ImGui::PushID(static_cast<int>(row.rowId));
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%u", row.rowId);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(row.name.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", row.modelPath.empty() ? PlacementStatus(row) : row.modelPath.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(TypeName(row.type));
            ImGui::TableNextColumn();
            if (row.custom) ImGui::Text("%zu-part M2", row.modelParts.size());
            else if (row.placeable) ImGui::Text("M2 %u", row.modelFdid);
            else ImGui::TextDisabled("%s", ModelTypeName(row.modelType));
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!row.placeable);
            if (ImGui::SmallButton("Place")) Place(row);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        clipper.End();
        ImGui::EndTable();
    }

    void Catalog::DrawPanel()
    {
        if (!ImGuiHostExt::Instance().Visible()) return;
        if (!EnsureLoaded())
        {
            ImGui::Begin("Housing - Catalog");
            ImGui::TextDisabled("Catalog unavailable (wxl-db2 missing?)");
            ImGui::End();
            return;
        }

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 workPos = viewport ? viewport->WorkPos : ImVec2(0.0f, 0.0f);
        const ImVec2 workSize = viewport ? viewport->WorkSize : ImGui::GetIO().DisplaySize;
        constexpr float reachableMargin = 12.0f;
        const ImVec2 maximumWindow(
            (std::max)(96.0f, workSize.x - reachableMargin * 2.0f),
            (std::max)(96.0f, workSize.y - reachableMargin * 2.0f));
        const ImVec2 minimumWindow(
            (std::min)(720.0f, maximumWindow.x),
            (std::min)(480.0f, maximumWindow.y));
        ImGui::SetNextWindowSizeConstraints(minimumWindow, maximumWindow);
        ImGui::SetNextWindowSize(ImVec2((std::min)(1060.0f, maximumWindow.x),
                                        (std::min)(740.0f, maximumWindow.y)),
                                 ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(workPos.x + 24.0f, workPos.y + 24.0f),
                                ImGuiCond_FirstUseEver);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.045f, 0.035f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.52f, 0.42f, 0.22f, 0.82f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.030f, 0.024f, 0.82f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::Begin("Housing Dashboard  (Insert toggles)");

        // Saved ImGui positions can be outside a newly-selected resolution, and live
        // dragging can move the title bar past an edge. Clamp every frame so at least
        // a small margin remains reachable after drags, resizes, or display changes.
        const ImVec2 windowSize = ImGui::GetWindowSize();
        const ImVec2 windowPos = ImGui::GetWindowPos();
        const float availableX = workSize.x - windowSize.x;
        const float availableY = workSize.y - windowSize.y;
        const float minX = workPos.x + (availableX >= reachableMargin * 2.0f ? reachableMargin : 0.0f);
        const float minY = workPos.y + (availableY >= reachableMargin * 2.0f ? reachableMargin : 0.0f);
        const float maxX = workPos.x + (std::max)(0.0f,
            availableX - (availableX >= reachableMargin * 2.0f ? reachableMargin : 0.0f));
        const float maxY = workPos.y + (std::max)(0.0f,
            availableY - (availableY >= reachableMargin * 2.0f ? reachableMargin : 0.0f));
        const ImVec2 clampedPos((std::max)(minX, (std::min)(windowPos.x, maxX)),
                                (std::max)(minY, (std::min)(windowPos.y, maxY)));
        if (std::fabs(clampedPos.x - windowPos.x) > 0.5f ||
            std::fabs(clampedPos.y - windowPos.y) > 0.5f)
            ImGui::SetWindowPos(clampedPos, ImGuiCond_Always);

        if (void* panelTexture = LoadTextureFile(kUiTextureCatalogPanel,
                "Extensions\\wxl-housing\\ui\\housingcatalogpanel.png"))
        {
            const ImVec2 pos = ImGui::GetWindowPos();
            const ImVec2 size = ImGui::GetWindowSize();
            // The first 552x480 pixels of the Retail atlas are the dark wood catalog panel.
            ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(panelTexture),
                ImVec2(pos.x + 3.0f, pos.y + 22.0f),
                ImVec2(pos.x + size.x - 3.0f, pos.y + size.y - 3.0f),
                ImVec2(0.0f, 0.0f), ImVec2(552.0f / 1024.0f, 480.0f / 512.0f),
                IM_COL32(255, 255, 255, 150));
        }

        const size_t placeable = std::count_if(filtered_.begin(), filtered_.end(),
            [](const DecorRow* r) { return r->placeable; });
        const float titleWidth = ImGui::CalcTextSize("HOUSING DASHBOARD").x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - titleWidth) * 0.5f);
        ImGui::TextColored(ImVec4(0.94f, 0.80f, 0.32f, 1.0f), "HOUSING DASHBOARD");
        ImGui::Separator();

        ImGui::SetNextItemWidth((std::max)(120.0f, ImGui::GetContentRegionAvail().x - 280.0f));
        if (ImGui::InputTextWithHint("##filter", "Search furniture, ID, or model path...", filter_, sizeof filter_))
            RebuildFiltered();
        ImGui::SameLine();
        if (ImGui::Button("FILTER", ImVec2(68.0f, 0.0f))) ImGui::OpenPopup("catalog-filter");
        ImGui::SameLine();
        if (ImGui::Button("Rescan Custom Props")) RescanCustomProps();
        if (ImGui::BeginPopup("catalog-filter"))
        {
            ImGui::TextDisabled("Catalog filters");
            if (ImGui::Checkbox("Placeable on this client", &placeableOnly_)) RebuildFiltered();
            if (ImGui::Checkbox("Include technical [DNT]", &includeDnt_)) RebuildFiltered();
            ImGui::Checkbox("Compact diagnostic list", &compactView_);
            ImGui::Separator();
            static const char* kTypes[] = {
                "All placement types", "Other", "Free-standing", "Wall", "Ceiling", "Floor / rug"
            };
            const int comboIndex = typeFilter_ + 1;
            if (ImGui::BeginCombo("Placement", kTypes[comboIndex]))
            {
                for (int i = 0; i < static_cast<int>(sizeof(kTypes) / sizeof(kTypes[0])); ++i)
                {
                    if (ImGui::Selectable(kTypes[i], comboIndex == i))
                    {
                        typeFilter_ = i - 1;
                        RebuildFiltered();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndPopup();
        }

        ImGui::TextDisabled("%zu shown / %zu known decor  |  %zu locally placeable  |  %zu custom manifests",
                            filtered_.size(), rows_.size(), placeable, customManifestCount_);
        if (!customScanErrors_.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.30f, 1.0f),
                               "| %zu manifest errors", customScanErrors_.size());
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                const size_t shown = (std::min)(customScanErrors_.size(), static_cast<size_t>(12));
                for (size_t i = 0; i < shown; ++i) ImGui::TextWrapped("%s", customScanErrors_[i].c_str());
                if (shown < customScanErrors_.size()) ImGui::TextDisabled("...and %zu more", customScanErrors_.size() - shown);
                ImGui::EndTooltip();
            }
        }

        if (ImGui::CollapsingHeader("BUILD TOOLS", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto& freeCamera = FreeBuildCamera::Instance();
            const bool cameraActive = freeCamera.Active();
            const bool cameraTransitioning = freeCamera.Transitioning();
            if (cameraActive)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.47f, 0.24f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.62f, 0.31f, 1.0f));
            }
            if (cameraTransitioning) ImGui::BeginDisabled();
            const char* cameraButton = cameraActive ? "EXIT FREE CAMERA" :
                (cameraTransitioning ? "CAMERA RESTORING..." : "FREE BUILD CAMERA");
            if (ImGui::Button(cameraButton, ImVec2(176.0f, 0.0f)) && !cameraTransitioning)
                freeCamera.Toggle();
            if (cameraTransitioning) ImGui::EndDisabled();
            if (cameraActive) ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("No-clip local build camera\n"
                                  "RMB look | WASD move | Q/Space up | E/Ctrl down\n"
                                  "Shift boosts | Esc exits | player stays in place");
            ImGui::SameLine();
            if (cameraActive)
            {
                ImGui::TextColored(ImVec4(0.40f, 0.86f, 0.48f, 1.0f),
                                   "%s", freeCamera.Status());
                ImGui::TextDisabled("%s", freeCamera.DiagnosticText());
            }
            else if (cameraTransitioning)
            {
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.26f, 1.0f),
                                   "%s", freeCamera.Status());
                ImGui::TextDisabled("%s", freeCamera.DiagnosticText());
            }
            else if (std::strstr(freeCamera.Status(), "ready"))
                ImGui::TextDisabled("%s", freeCamera.Status());
            else
                ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.30f, 1.0f),
                                   "CAMERA ERROR: %s", freeCamera.Status());

            ImGui::Separator();
            TerrainDeform::Instance().DrawControls();
        }
        const float bodyHeight = (std::max)(220.0f,
            (std::min)(598.0f, ImGui::GetContentRegionAvail().y - 30.0f));
        if (compactView_)
        {
            DrawCompactTable(bodyHeight);
        }
        else
        {
            DrawCategoryRail(bodyHeight);
            ImGui::SameLine();
            DrawGrid(bodyHeight);
            ImGui::SameLine();
            DrawDetails(bodyHeight);
        }

        ImGui::Separator();
        ImGui::TextDisabled("Catalog selects a preview; PLACE DECOR spawns in front of you. Left-click placed decor to edit it.");
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
    }

    void Catalog::OnDeviceLost()
    {
        for (auto& pair : thumbnails_)
            if (pair.second) reinterpret_cast<IDirect3DTexture9*>(pair.second)->Release();
        thumbnails_.clear();
        for (auto& pair : customThumbnails_)
            if (pair.second) reinterpret_cast<IDirect3DTexture9*>(pair.second)->Release();
        customThumbnails_.clear();
        device_ = nullptr;
    }

    void Catalog::Frame(void* device)
    {
        device_ = device;
        thumbnailLoadsRemaining_ = 6; // avoid a long hitch when scrolling into uncached cards
        DrawPanel();
    }
}
