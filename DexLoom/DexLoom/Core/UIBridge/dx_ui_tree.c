#include "../Include/dx_view.h"
#include "../Include/dx_log.h"
#include "../Include/dx_context.h"
#include "../Include/dx_resources.h"
#include "../Include/dx_apk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define TAG "UITree"

#include "../Include/dx_memory.h"

// ============================================================
// Dimension unit conversion
// ============================================================

// Screen density scale factor.
// On iOS, 1pt = 1dp for @2x retina displays (160dpi baseline × 2 = 320dpi).
// For @3x (e.g. Plus/Max models, 160 × 3 = 480dpi), dp values still map 1:1
// to iOS points because Apple's point system already accounts for density.
// Therefore the dp→pt factor is always 1.0 on iOS.
#define DX_DP_SCALE 1.0f

// Convert Android dp (density-independent pixels) to iOS points.
// On iOS @2x/3x, 1dp ≈ 1pt because both systems use 160dpi as baseline.
float dx_ui_dp_to_points(float dp) {
    return dp * DX_DP_SCALE;
}

// Convert Android sp (scale-independent pixels for text) to iOS points.
// At default user text scale, 1sp = 1dp = 1pt on iOS.
float dx_ui_sp_to_points(float sp) {
    return sp * DX_DP_SCALE;
}

// Convert Android px (raw pixels) to iOS points.
// Assuming source density of ~320dpi (mdpi × 2), divide by 2.0 to get points.
// This matches the common xxhdpi resource bucket (480dpi / 3 = 160 = mdpi baseline).
#define DX_PX_TO_PT_SCALE 0.5f
static float dx_ui_px_to_points(float px) {
    return px * DX_PX_TO_PT_SCALE;
}

// Decode a complex Android dimension attribute value and convert to iOS points.
// Uses the proper Android fixed-point format decoder from dx_resources.
static float dx_ui_decode_dimension(uint32_t attr_data) {
    uint8_t unit = 0;
    float value = dx_resources_decode_dimen(attr_data, &unit);

    switch (unit) {
        case DX_DIMEN_UNIT_PX:
            return dx_ui_px_to_points(value);
        case DX_DIMEN_UNIT_DIP: // dp
            return dx_ui_dp_to_points(value);
        case DX_DIMEN_UNIT_SP:
            return dx_ui_sp_to_points(value);
        case DX_DIMEN_UNIT_PT:
            return value; // pt → pt, 1:1
        case DX_DIMEN_UNIT_IN:
            return value * 72.0f; // 1 inch = 72 points
        case DX_DIMEN_UNIT_MM:
            return value * 72.0f / 25.4f; // mm → points
        default:
            return dx_ui_dp_to_points(value); // fallback: treat as dp
    }
}

// Convenience: decode a dimension attribute and return as int32_t (rounded).
static int32_t dx_ui_decode_dimension_int(uint32_t attr_data) {
    float pts = dx_ui_decode_dimension(attr_data);
    return (int32_t)(pts + 0.5f);
}

DxUINode *dx_ui_node_create(DxViewType type, uint32_t view_id) {
    DxUINode *node = (DxUINode *)dx_malloc(sizeof(DxUINode));
    if (!node) return NULL;

    node->type = type;
    node->view_id = view_id;
    node->visibility = DX_VISIBLE;
    node->orientation = DX_ORIENTATION_VERTICAL;
    node->text_size = 16.0f;
    node->width = -1;   // match_parent
    node->height = -2;  // wrap_content

    // Initialize ConstraintLayout constraints to "none"
    node->constraints.left_to_left = DX_CONSTRAINT_NONE;
    node->constraints.left_to_right = DX_CONSTRAINT_NONE;
    node->constraints.right_to_right = DX_CONSTRAINT_NONE;
    node->constraints.right_to_left = DX_CONSTRAINT_NONE;
    node->constraints.top_to_top = DX_CONSTRAINT_NONE;
    node->constraints.top_to_bottom = DX_CONSTRAINT_NONE;
    node->constraints.bottom_to_bottom = DX_CONSTRAINT_NONE;
    node->constraints.bottom_to_top = DX_CONSTRAINT_NONE;
    node->constraints.horizontal_bias = 0.5f;
    node->constraints.vertical_bias = 0.5f;

    return node;
}

void dx_ui_node_destroy(DxUINode *node) {
    if (!node) return;

    for (uint32_t i = 0; i < node->child_count; i++) {
        dx_ui_node_destroy(node->children[i]);
    }
    dx_free(node->children);
    dx_free(node->text);
    dx_free(node->hint);
    dx_free(node->image_data);
    dx_free(node->web_url);
    dx_free(node->web_html);
    if (node->draw_commands) {
        for (uint32_t i = 0; i < node->draw_cmd_count; i++) {
            dx_free(node->draw_commands[i].text);
        }
        dx_free(node->draw_commands);
    }
    dx_free(node);
}

void dx_ui_node_add_child(DxUINode *parent, DxUINode *child) {
    if (!parent || !child) return;

    if (parent->child_count >= parent->child_capacity) {
        uint32_t new_cap = parent->child_capacity == 0 ? 4 : parent->child_capacity * 2;
        DxUINode **new_children = (DxUINode **)dx_realloc(parent->children,
                                                           sizeof(DxUINode *) * new_cap);
        if (!new_children) return;
        parent->children = new_children;
        parent->child_capacity = new_cap;
    }

    child->parent = parent;
    parent->children[parent->child_count++] = child;
}

DxUINode *dx_ui_node_find_by_id(DxUINode *root, uint32_t view_id) {
    if (!root) return NULL;
    if (root->view_id == view_id && view_id != 0) return root;

    for (uint32_t i = 0; i < root->child_count; i++) {
        DxUINode *found = dx_ui_node_find_by_id(root->children[i], view_id);
        if (found) return found;
    }
    return NULL;
}

void dx_ui_node_set_text(DxUINode *node, const char *text) {
    if (!node) return;
    dx_free(node->text);
    node->text = text ? dx_strdup(text) : NULL;
}

uint32_t dx_ui_node_count(const DxUINode *node) {
    if (!node) return 0;
    uint32_t count = 1;
    for (uint32_t i = 0; i < node->child_count; i++) {
        count += dx_ui_node_count(node->children[i]);
    }
    return count;
}

// Count nodes of specific view types (for heuristic layout selection)
static void count_view_types(const DxUINode *node, uint32_t *buttons, uint32_t *text_views) {
    if (!node) return;
    if (node->type == DX_VIEW_BUTTON) (*buttons)++;
    if (node->type == DX_VIEW_TEXT_VIEW) (*text_views)++;
    for (uint32_t i = 0; i < node->child_count; i++) {
        count_view_types(node->children[i], buttons, text_views);
    }
}

uint32_t dx_ui_node_score_layout(const DxUINode *root) {
    if (!root) return 0;
    // Primary metric: total node count. The main UI layout typically has
    // the most views. Custom view classes (parsed as VIEW_GROUP) are
    // counted equally since apps use custom views for their UIs.
    return dx_ui_node_count(root);
}

// ============================================================
// UI tree inspector
// ============================================================

static const char *view_type_name(DxViewType type) {
    switch (type) {
        case DX_VIEW_NONE:              return "None";
        case DX_VIEW_LINEAR_LAYOUT:     return "LinearLayout";
        case DX_VIEW_TEXT_VIEW:          return "TextView";
        case DX_VIEW_BUTTON:            return "Button";
        case DX_VIEW_IMAGE_VIEW:        return "ImageView";
        case DX_VIEW_EDIT_TEXT:          return "EditText";
        case DX_VIEW_FRAME_LAYOUT:      return "FrameLayout";
        case DX_VIEW_RELATIVE_LAYOUT:   return "RelativeLayout";
        case DX_VIEW_CONSTRAINT_LAYOUT: return "ConstraintLayout";
        case DX_VIEW_SCROLL_VIEW:       return "ScrollView";
        case DX_VIEW_RECYCLER_VIEW:     return "RecyclerView";
        case DX_VIEW_CARD_VIEW:         return "CardView";
        case DX_VIEW_SWITCH:            return "Switch";
        case DX_VIEW_CHECKBOX:          return "CheckBox";
        case DX_VIEW_PROGRESS_BAR:      return "ProgressBar";
        case DX_VIEW_TOOLBAR:           return "Toolbar";
        case DX_VIEW_VIEW:              return "View";
        case DX_VIEW_VIEW_GROUP:        return "ViewGroup";
        case DX_VIEW_LIST_VIEW:         return "ListView";
        case DX_VIEW_GRID_VIEW:         return "GridView";
        case DX_VIEW_SPINNER:           return "Spinner";
        case DX_VIEW_SEEK_BAR:          return "SeekBar";
        case DX_VIEW_RATING_BAR:        return "RatingBar";
        case DX_VIEW_RADIO_BUTTON:      return "RadioButton";
        case DX_VIEW_RADIO_GROUP:       return "RadioGroup";
        case DX_VIEW_FAB:               return "FAB";
        case DX_VIEW_TAB_LAYOUT:        return "TabLayout";
        case DX_VIEW_VIEW_PAGER:        return "ViewPager";
        case DX_VIEW_WEB_VIEW:          return "WebView";
        case DX_VIEW_CHIP:              return "Chip";
        case DX_VIEW_BOTTOM_NAV:        return "BottomNav";
        case DX_VIEW_SWIPE_REFRESH:     return "SwipeRefresh";
        default:                        return "Unknown";
    }
}

// Dynamic string buffer for tree dump
typedef struct {
    char    *data;
    uint32_t len;
    uint32_t cap;
} DxStrBuf;

static void strbuf_init(DxStrBuf *sb) {
    sb->cap = 1024;
    sb->data = (char *)dx_malloc(sb->cap);
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

static void strbuf_appendf(DxStrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void strbuf_appendf(DxStrBuf *sb, const char *fmt, ...) {
    if (!sb->data) return;
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) return;

    while (sb->len + (uint32_t)needed + 1 > sb->cap) {
        uint32_t new_cap = sb->cap * 2;
        char *new_data = (char *)dx_realloc(sb->data, new_cap);
        if (!new_data) return;
        sb->data = new_data;
        sb->cap = new_cap;
    }

    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    sb->len += (uint32_t)needed;
}

static void dump_node(DxStrBuf *sb, DxUINode *node, int depth) {
    if (!node) return;

    // Indentation
    for (int i = 0; i < depth; i++) {
        strbuf_appendf(sb, "  ");
    }

    // Node info
    strbuf_appendf(sb, "%s", view_type_name(node->type));
    if (node->view_id != 0) {
        strbuf_appendf(sb, " id=0x%x", node->view_id);
    }
    if (node->text) {
        if (strlen(node->text) > 30) {
            strbuf_appendf(sb, " text=\"%.27s...\"", node->text);
        } else {
            strbuf_appendf(sb, " text=\"%s\"", node->text);
        }
    }
    strbuf_appendf(sb, " w=%d h=%d", node->width, node->height);
    if (node->child_count > 0) {
        strbuf_appendf(sb, " children=%u", node->child_count);
    }
    if (node->visibility == DX_GONE) {
        strbuf_appendf(sb, " [GONE]");
    } else if (node->visibility == DX_INVISIBLE) {
        strbuf_appendf(sb, " [INVISIBLE]");
    }
    strbuf_appendf(sb, "\n");

    for (uint32_t i = 0; i < node->child_count; i++) {
        dump_node(sb, node->children[i], depth + 1);
    }
}

char *dx_ui_tree_dump(DxUINode *root) {
    if (!root) {
        char *empty = dx_strdup("(no UI tree)\n");
        return empty;
    }

    DxStrBuf sb;
    strbuf_init(&sb);

    uint32_t total = dx_ui_node_count(root);
    strbuf_appendf(&sb, "UI Tree (%u nodes):\n", total);
    dump_node(&sb, root, 0);

    return sb.data;  // caller must free
}

// ============================================================
// Render model (serialized snapshot for Swift bridge)
// ============================================================

static DxRenderNode *serialize_node(DxUINode *node) {
    if (!node) return NULL;

    DxRenderNode *rn = (DxRenderNode *)dx_malloc(sizeof(DxRenderNode));
    if (!rn) return NULL;

    rn->type = node->type;
    rn->view_id = node->view_id;
    rn->visibility = node->visibility;
    rn->text = node->text ? dx_strdup(node->text) : NULL;
    rn->hint = node->hint ? dx_strdup(node->hint) : NULL;
    rn->orientation = node->orientation;
    rn->text_size = node->text_size;
    rn->width = node->width;
    rn->height = node->height;
    rn->weight = node->weight;
    rn->gravity = node->gravity;
    memcpy(rn->padding, node->padding, sizeof(rn->padding));
    memcpy(rn->margin, node->margin, sizeof(rn->margin));
    rn->bg_color = node->bg_color;
    rn->text_color = node->text_color;
    rn->is_checked = node->is_checked;
    rn->has_click_listener = (node->click_listener != NULL);
    rn->has_long_click_listener = (node->long_click_listener != NULL);
    rn->has_refresh_listener = (node->refresh_listener != NULL);
    rn->relative_flags = node->relative_flags;
    rn->rel_above = node->rel_above;
    rn->rel_below = node->rel_below;
    rn->rel_left_of = node->rel_left_of;
    rn->rel_right_of = node->rel_right_of;
    rn->constraints = node->constraints;
    rn->image_data = node->image_data;        // borrow pointer (DxUINode owns the data)
    rn->image_data_len = node->image_data_len;
    rn->web_url = node->web_url ? dx_strdup(node->web_url) : NULL;
    rn->web_html = node->web_html ? dx_strdup(node->web_html) : NULL;

    // Copy draw commands
    if (node->draw_cmd_count > 0 && node->draw_commands) {
        rn->draw_cmd_count = node->draw_cmd_count;
        rn->draw_commands = (DxDrawCommand *)dx_malloc(sizeof(DxDrawCommand) * node->draw_cmd_count);
        if (rn->draw_commands) {
            memcpy(rn->draw_commands, node->draw_commands, sizeof(DxDrawCommand) * node->draw_cmd_count);
            // Deep-copy text strings
            for (uint32_t i = 0; i < rn->draw_cmd_count; i++) {
                if (rn->draw_commands[i].text) {
                    rn->draw_commands[i].text = dx_strdup(rn->draw_commands[i].text);
                }
            }
        } else {
            rn->draw_cmd_count = 0;
        }
    } else {
        rn->draw_commands = NULL;
        rn->draw_cmd_count = 0;
    }

    if (node->child_count > 0) {
        rn->children = (DxRenderNode *)dx_malloc(sizeof(DxRenderNode) * node->child_count);
        rn->child_count = node->child_count;
        for (uint32_t i = 0; i < node->child_count; i++) {
            DxRenderNode *child = serialize_node(node->children[i]);
            if (child) {
                rn->children[i] = *child;
                dx_free(child);
            }
        }
    }

    return rn;
}

DxRenderModel *dx_render_model_create(DxUINode *root) {
    if (!root) return NULL;

    DxRenderModel *model = (DxRenderModel *)dx_malloc(sizeof(DxRenderModel));
    if (!model) return NULL;

    static uint32_t version_counter = 0;
    model->version = ++version_counter;
    model->root = serialize_node(root);

    DX_DEBUG(TAG, "Render model v%u created", model->version);
    return model;
}

static void free_render_node(DxRenderNode *node) {
    if (!node) return;
    dx_free(node->text);
    dx_free(node->hint);
    dx_free(node->web_url);
    dx_free(node->web_html);
    if (node->draw_commands) {
        for (uint32_t i = 0; i < node->draw_cmd_count; i++) {
            dx_free(node->draw_commands[i].text);
        }
        dx_free(node->draw_commands);
    }
    for (uint32_t i = 0; i < node->child_count; i++) {
        free_render_node(&node->children[i]);
    }
    dx_free(node->children);
}

void dx_render_model_destroy(DxRenderModel *model) {
    if (!model) return;
    if (model->root) {
        free_render_node(model->root);
        dx_free(model->root);
    }
    dx_free(model);
}

// ============================================================
// Layout XML parser -> UI tree
// ============================================================

// Minimal AXML parser for layout files
// Reuses the same binary XML format as AndroidManifest

#define AXML_FILE_MAGIC        0x00080003
#define AXML_CHUNK_STRINGPOOL  0x0001
#define AXML_CHUNK_RESOURCEMAP 0x0180
#define AXML_CHUNK_START_TAG   0x0102
#define AXML_CHUNK_END_TAG     0x0103
#define AXML_MAX_STRING_POOL   1000000  // 1M strings max
#define AXML_MAX_NESTING_DEPTH 100

// Well-known attribute resource IDs for layout
#define ATTR_ID          0x010100d0
#define ATTR_TEXT         0x01010014
#define ATTR_ORIENTATION  0x010100c4
#define ATTR_GRAVITY      0x010100af
#define ATTR_HINT         0x01010150
#define ATTR_TEXT_COLOR   0x01010098
#define ATTR_TEXT_SIZE    0x01010095
#define ATTR_BACKGROUND   0x010100d4
#define ATTR_PADDING      0x010100d5
#define ATTR_PADDING_L    0x010100d6
#define ATTR_PADDING_T    0x010100d7
#define ATTR_PADDING_R    0x010100d8
#define ATTR_PADDING_B    0x010100d9
#define ATTR_VISIBILITY   0x010100dc
#define ATTR_CHECKED      0x01010108
#define ATTR_LAYOUT_WIDTH  0x010100f4
#define ATTR_LAYOUT_HEIGHT 0x010100f5
#define ATTR_LAYOUT_MARGIN 0x010100f6
#define ATTR_LAYOUT_MARGIN_L 0x010100f7
#define ATTR_LAYOUT_MARGIN_T 0x010100f8
#define ATTR_LAYOUT_MARGIN_R 0x010100f9
#define ATTR_LAYOUT_MARGIN_B 0x010100fa
#define ATTR_PADDING_START 0x010103b3
#define ATTR_PADDING_END   0x010103b4
#define ATTR_LAYOUT_WEIGHT 0x01010181
#define ATTR_LAYOUT_MARGIN_START 0x010103b5
#define ATTR_LAYOUT_MARGIN_END   0x010103b6
#define ATTR_SRC           0x01010119  // android:src (ImageView drawable)

// Style attribute
#define ATTR_STYLE         0x010100ba  // android:style (but style is often attr index 0xFFFFFFFF)

// Well-known theme attribute IDs (android.R.attr.*)
#define ATTR_COLOR_PRIMARY       0x01010433
#define ATTR_COLOR_PRIMARY_DARK  0x01010434
#define ATTR_COLOR_ACCENT        0x01010435
#define ATTR_COLOR_BACKGROUND    0x010100d4  // same as android:background
#define ATTR_TEXT_COLOR_PRIMARY   0x01010036
#define ATTR_WINDOW_BACKGROUND   0x01010054

// Attribute reference type (for ?attr/ values)
#define RES_VALUE_TYPE_ATTR  0x02

// RelativeLayout attributes
#define ATTR_LAYOUT_ABOVE            0x01010140
#define ATTR_LAYOUT_BELOW            0x01010141
#define ATTR_LAYOUT_TO_LEFT_OF       0x01010142
#define ATTR_LAYOUT_TO_RIGHT_OF      0x01010143
#define ATTR_LAYOUT_ALIGN_PARENT_TOP    0x01010130
#define ATTR_LAYOUT_ALIGN_PARENT_BOTTOM 0x01010131
#define ATTR_LAYOUT_ALIGN_PARENT_LEFT   0x01010132
#define ATTR_LAYOUT_ALIGN_PARENT_RIGHT  0x01010133
#define ATTR_LAYOUT_CENTER_IN_PARENT    0x01010134
#define ATTR_LAYOUT_CENTER_HORIZONTAL   0x01010135
#define ATTR_LAYOUT_CENTER_VERTICAL     0x01010136

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

// Try to extract a drawable image from the APK for a given resource ID.
// Looks up the resource entry to get the file path, then extracts from APK.
// Returns allocated image bytes (caller owns), or NULL on failure.
static uint8_t *dx_ui_extract_drawable(DxContext *ctx, uint32_t res_id, uint32_t *out_len) {
    if (!ctx || !ctx->resources || !ctx->apk || !out_len) return NULL;

    // Look up the resource entry to get the drawable file path
    const DxResourceEntry *entry = dx_resources_find_by_id(ctx->resources, res_id);
    if (!entry) return NULL;

    // The entry should be a string type pointing to a file path like "res/drawable-hdpi/icon.png"
    const char *path = NULL;
    if (entry->value_type == DX_RES_TYPE_STRING && entry->str_val) {
        path = entry->str_val;
    }
    if (!path) return NULL;

    // Only extract actual image files (PNG, JPEG, WEBP)
    size_t plen = strlen(path);
    bool is_image = false;
    if (plen > 4) {
        const char *ext = path + plen - 4;
        if (strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0 ||
            strcmp(ext, ".PNG") == 0 || strcmp(ext, ".JPG") == 0) {
            is_image = true;
        }
        if (plen > 5) {
            ext = path + plen - 5;
            if (strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".JPEG") == 0 ||
                strcmp(ext, ".webp") == 0) {
                is_image = true;
            }
        }
    }
    if (!is_image) return NULL;

    // Find and extract the entry from the APK
    const DxZipEntry *zip_entry = NULL;
    if (dx_apk_find_entry(ctx->apk, path, &zip_entry) != DX_OK) {
        // Try density-qualified variants: drawable-hdpi, drawable-xhdpi, drawable-xxhdpi, etc.
        // The resource might point to a specific density but the APK may have others
        return NULL;
    }

    uint8_t *data = NULL;
    uint32_t size = 0;
    if (dx_apk_extract_entry(ctx->apk, zip_entry, &data, &size) != DX_OK) {
        return NULL;
    }

    DX_INFO(TAG, "Extracted drawable: %s (%u bytes) for res 0x%08x", path, size, res_id);
    *out_len = size;
    return data;
}

// ============================================================
// Style / theme attribute application
// ============================================================

// Resolve a ?attr/ reference through the context's theme.
// If value_type is 0x02 (attribute reference), look up in the theme.
// Modifies value_type and value_data in-place to the resolved concrete value.
static void resolve_attr_reference(DxContext *ctx, uint8_t *value_type, uint32_t *value_data) {
    if (!ctx || !ctx->theme || *value_type != RES_VALUE_TYPE_ATTR) return;

    const DxStyleEntry *resolved = dx_theme_resolve_attr(ctx->theme, *value_data);
    if (resolved) {
        // If the resolved value is itself a reference, try one more level
        if (resolved->value_type == 0x01 && ctx->resources) {
            const DxResourceEntry *re = dx_resources_find_by_id(ctx->resources, resolved->value_data);
            if (re) {
                *value_type = re->value_type;
                *value_data = (re->value_type == 0x1C || re->value_type == 0x1D ||
                               re->value_type == 0x1E || re->value_type == 0x1F)
                              ? re->color_val
                              : (uint32_t)re->int_val;
                return;
            }
        }
        *value_type = resolved->value_type;
        *value_data = resolved->value_data;
    }
}

// Apply a single style/theme attribute to a UI node.
// Only applies if the node doesn't already have a non-default value for that attribute
// (direct attributes take precedence over style attributes).
static void apply_style_attr_to_node(DxUINode *node, DxContext *ctx,
                                      uint32_t attr_id, uint8_t val_type, uint32_t val_data,
                                      char **strings, uint32_t string_count) {
    // Resolve ?attr/ references before applying
    resolve_attr_reference(ctx, &val_type, &val_data);

    switch (attr_id) {
        case ATTR_TEXT_COLOR:
            if (node->text_color == 0) { // not already set
                if (val_type == 0x1C || val_type == 0x1D || val_type == 0x1E || val_type == 0x1F) {
                    node->text_color = val_data;
                }
            }
            break;

        case ATTR_TEXT_SIZE:
            if (node->text_size == 16.0f) { // still default
                if (val_type == 0x05) {
                    float val = dx_ui_decode_dimension(val_data);
                    if (val > 0 && val < 200) node->text_size = val;
                }
            }
            break;

        case ATTR_BACKGROUND:
            if (node->bg_color == 0) {
                if (val_type == 0x1C || val_type == 0x1D || val_type == 0x1E || val_type == 0x1F) {
                    node->bg_color = val_data;
                }
            }
            break;

        case ATTR_ORIENTATION:
            // Only apply if still default vertical
            if (node->orientation == DX_ORIENTATION_VERTICAL) {
                node->orientation = (val_data == 1) ? DX_ORIENTATION_VERTICAL :
                                                       DX_ORIENTATION_HORIZONTAL;
            }
            break;

        case ATTR_GRAVITY:
            if (node->gravity == 0) {
                node->gravity = (int32_t)val_data;
            }
            break;

        case ATTR_PADDING:
            if (node->padding[0] == 0 && node->padding[1] == 0 &&
                node->padding[2] == 0 && node->padding[3] == 0) {
                int32_t pts = (val_type == 0x05) ? dx_ui_decode_dimension_int(val_data) : (int32_t)val_data;
                node->padding[0] = node->padding[1] = node->padding[2] = node->padding[3] = pts;
            }
            break;

        case ATTR_PADDING_L:
            if (node->padding[0] == 0) {
                node->padding[0] = (val_type == 0x05) ? dx_ui_decode_dimension_int(val_data) : (int32_t)val_data;
            }
            break;
        case ATTR_PADDING_T:
            if (node->padding[1] == 0) {
                node->padding[1] = (val_type == 0x05) ? dx_ui_decode_dimension_int(val_data) : (int32_t)val_data;
            }
            break;
        case ATTR_PADDING_R:
            if (node->padding[2] == 0) {
                node->padding[2] = (val_type == 0x05) ? dx_ui_decode_dimension_int(val_data) : (int32_t)val_data;
            }
            break;
        case ATTR_PADDING_B:
            if (node->padding[3] == 0) {
                node->padding[3] = (val_type == 0x05) ? dx_ui_decode_dimension_int(val_data) : (int32_t)val_data;
            }
            break;

        case ATTR_LAYOUT_MARGIN:
            if (node->margin[0] == 0 && node->margin[1] == 0 &&
                node->margin[2] == 0 && node->margin[3] == 0) {
                int32_t pts = (val_type == 0x05) ? dx_ui_decode_dimension_int(val_data) : (int32_t)val_data;
                node->margin[0] = node->margin[1] = node->margin[2] = node->margin[3] = pts;
            }
            break;

        case ATTR_LAYOUT_WIDTH:
            // Don't override if already set to something explicit
            break;
        case ATTR_LAYOUT_HEIGHT:
            break;

        case ATTR_VISIBILITY:
            if (node->visibility == DX_VISIBLE) {
                if (val_data == 4) node->visibility = DX_INVISIBLE;
                else if (val_data == 8) node->visibility = DX_GONE;
            }
            break;

        default:
            // Ignore attributes we don't handle yet
            break;
    }
}

// Apply all attributes from a resolved style bag to a node (as defaults).
// Direct XML attributes that were already parsed take precedence.
static void apply_style_to_node(DxUINode *node, DxContext *ctx,
                                 const DxStyleBag *style,
                                 char **strings, uint32_t string_count) {
    if (!style || !node) return;
    for (uint32_t i = 0; i < style->entry_count; i++) {
        apply_style_attr_to_node(node, ctx,
                                  style->entries[i].attr_id,
                                  style->entries[i].value_type,
                                  style->entries[i].value_data,
                                  strings, string_count);
    }
}

DxResult dx_layout_parse(DxContext *ctx, const uint8_t *xml_data, uint32_t xml_size,
                           DxUINode **out) {
    if (!xml_data || !out) return DX_ERR_NULL_PTR;
    if (xml_size < 8) return DX_ERR_AXML_INVALID;

    uint32_t magic = read_u32(xml_data);
    if (magic != AXML_FILE_MAGIC) {
        DX_ERROR(TAG, "Invalid layout AXML magic");
        return DX_ERR_AXML_INVALID;
    }

    // Parse string pool
    uint32_t pos = 8;
    char **strings = NULL;
    uint32_t string_count = 0;
    uint32_t *res_ids = NULL;
    uint32_t res_id_count = 0;

    if (pos + 8 > xml_size) return DX_ERR_AXML_INVALID;

    uint16_t chunk_type = read_u16(xml_data + pos);
    uint32_t chunk_size = read_u32(xml_data + pos + 4);

    if (chunk_type == AXML_CHUNK_STRINGPOOL && pos + chunk_size <= xml_size) {
        string_count = read_u32(xml_data + pos + 8);
        if (string_count > AXML_MAX_STRING_POOL) {
            DX_WARN(TAG, "AXML string pool too large: %u strings (max %u)",
                    string_count, AXML_MAX_STRING_POOL);
            return DX_ERR_AXML_INVALID;
        }
        uint32_t flags = read_u32(xml_data + pos + 16);
        uint32_t strings_start = read_u32(xml_data + pos + 20);
        bool is_utf8 = (flags & (1 << 8)) != 0;

        strings = (char **)dx_malloc(sizeof(char *) * string_count);
        uint32_t offsets_start = pos + 28;
        uint32_t pool_data_start = pos + strings_start;

        for (uint32_t i = 0; i < string_count && strings; i++) {
            if (offsets_start + i * 4 + 4 > xml_size) break;
            uint32_t str_off = read_u32(xml_data + offsets_start + i * 4);
            const uint8_t *sp = xml_data + pool_data_start + str_off;
            if (is_utf8) {
                uint32_t char_count = *sp++;
                if (char_count > 0x7F) { char_count = ((char_count & 0x7F) << 8) | *sp++; }
                uint32_t byte_count = *sp++;
                if (byte_count > 0x7F) { byte_count = ((byte_count & 0x7F) << 8) | *sp++; }
                strings[i] = (char *)dx_malloc(byte_count + 1);
                if (strings[i]) { memcpy(strings[i], sp, byte_count); strings[i][byte_count] = 0; }
            } else {
                uint16_t cc = read_u16(sp); sp += 2;
                strings[i] = (char *)dx_malloc(cc + 1);
                if (strings[i]) {
                    for (uint32_t j = 0; j < cc; j++) {
                        uint16_t c = read_u16(sp + j * 2);
                        strings[i][j] = (c < 128) ? (char)c : '?';
                    }
                    strings[i][cc] = 0;
                }
            }
        }
        pos += chunk_size;
    }

    // Resource map
    if (pos + 8 <= xml_size && read_u16(xml_data + pos) == AXML_CHUNK_RESOURCEMAP) {
        chunk_size = read_u32(xml_data + pos + 4);
        res_id_count = (chunk_size - 8) / 4;
        res_ids = (uint32_t *)dx_malloc(sizeof(uint32_t) * res_id_count);
        for (uint32_t i = 0; i < res_id_count && res_ids; i++) {
            res_ids[i] = read_u32(xml_data + pos + 8 + i * 4);
        }
        pos += chunk_size;
    }

    // Walk XML tags to build UI tree
    DxUINode *root = NULL;
    DxUINode *stack[AXML_MAX_NESTING_DEPTH];
    int stack_depth = 0;

    while (pos + 8 <= xml_size) {
        chunk_type = read_u16(xml_data + pos);
        chunk_size = read_u32(xml_data + pos + 4);
        if (chunk_size < 8 || pos + chunk_size > xml_size) break;

        if (chunk_type == AXML_CHUNK_START_TAG) {
            if (pos + 36 > xml_size) break;
            uint32_t name_idx = read_u32(xml_data + pos + 20);
            uint16_t attr_count = read_u16(xml_data + pos + 28);

            const char *tag = (name_idx < string_count && strings) ? strings[name_idx] : "";

            DxViewType vtype = DX_VIEW_NONE;
            if (strcmp(tag, "LinearLayout") == 0) vtype = DX_VIEW_LINEAR_LAYOUT;
            else if (strcmp(tag, "TextView") == 0) vtype = DX_VIEW_TEXT_VIEW;
            else if (strcmp(tag, "Button") == 0) vtype = DX_VIEW_BUTTON;
            else if (strcmp(tag, "ImageView") == 0) vtype = DX_VIEW_IMAGE_VIEW;
            else if (strcmp(tag, "EditText") == 0) vtype = DX_VIEW_EDIT_TEXT;
            else if (strcmp(tag, "FrameLayout") == 0) vtype = DX_VIEW_FRAME_LAYOUT;
            else if (strcmp(tag, "RelativeLayout") == 0) vtype = DX_VIEW_RELATIVE_LAYOUT;
            else if (strcmp(tag, "ScrollView") == 0 ||
                     strcmp(tag, "HorizontalScrollView") == 0 ||
                     strcmp(tag, "NestedScrollView") == 0) vtype = DX_VIEW_SCROLL_VIEW;
            else if (strcmp(tag, "Switch") == 0 ||
                     strcmp(tag, "SwitchCompat") == 0) vtype = DX_VIEW_SWITCH;
            else if (strcmp(tag, "CheckBox") == 0) vtype = DX_VIEW_CHECKBOX;
            else if (strcmp(tag, "RadioButton") == 0) vtype = DX_VIEW_RADIO_BUTTON;
            else if (strcmp(tag, "ProgressBar") == 0) vtype = DX_VIEW_PROGRESS_BAR;
            else if (strcmp(tag, "Toolbar") == 0) vtype = DX_VIEW_TOOLBAR;
            else if (strcmp(tag, "Spinner") == 0) vtype = DX_VIEW_SPINNER;
            else if (strcmp(tag, "SeekBar") == 0) vtype = DX_VIEW_SEEK_BAR;
            else if (strcmp(tag, "RatingBar") == 0) vtype = DX_VIEW_RATING_BAR;
            else if (strcmp(tag, "WebView") == 0) vtype = DX_VIEW_WEB_VIEW;
            else if (strcmp(tag, "View") == 0 ||
                     strcmp(tag, "Space") == 0) vtype = DX_VIEW_VIEW;
            else if (strstr(tag, "RecyclerView") != NULL) vtype = DX_VIEW_RECYCLER_VIEW;
            else if (strstr(tag, "CardView") != NULL) vtype = DX_VIEW_CARD_VIEW;
            else if (strstr(tag, "ConstraintLayout") != NULL) vtype = DX_VIEW_CONSTRAINT_LAYOUT;
            else if (strstr(tag, "FloatingActionButton") != NULL) vtype = DX_VIEW_FAB;
            else if (strstr(tag, "TabLayout") != NULL) vtype = DX_VIEW_TAB_LAYOUT;
            else if (strstr(tag, "ViewPager") != NULL) vtype = DX_VIEW_VIEW_PAGER;
            else if (strstr(tag, "Chip") != NULL && strstr(tag, "ChipGroup") == NULL) vtype = DX_VIEW_CHIP;
            else if (strstr(tag, "BottomNavigationView") != NULL) vtype = DX_VIEW_BOTTOM_NAV;
            else if (strstr(tag, "SwipeRefreshLayout") != NULL) vtype = DX_VIEW_SWIPE_REFRESH;
            else if (strstr(tag, "RadioGroup") != NULL) vtype = DX_VIEW_RADIO_GROUP;
            else if (strstr(tag, "CoordinatorLayout") != NULL ||
                     strstr(tag, "AppBarLayout") != NULL ||
                     strstr(tag, "CollapsingToolbarLayout") != NULL ||
                     strstr(tag, "DrawerLayout") != NULL ||
                     strstr(tag, "NavigationView") != NULL ||
                     strstr(tag, "Layout") != NULL) vtype = DX_VIEW_VIEW_GROUP;
            // Any remaining unrecognized tags: treat as generic container
            if (vtype == DX_VIEW_NONE) vtype = DX_VIEW_VIEW_GROUP;

            DxUINode *node = dx_ui_node_create(vtype, 0);
            if (!node) break;

            // Track style resource ID for this element
            uint32_t style_res_id = 0;

            // Parse attributes
            uint32_t attr_start = pos + 36;
            for (uint16_t a = 0; a < attr_count; a++) {
                uint32_t aoff = attr_start + a * 20;
                if (aoff + 20 > xml_size) break;

                uint32_t attr_name_idx = read_u32(xml_data + aoff + 4);
                uint32_t attr_raw = read_u32(xml_data + aoff + 8);
                uint32_t attr_typed = read_u32(xml_data + aoff + 12);
                uint32_t attr_data = read_u32(xml_data + aoff + 16);
                uint32_t attr_type = attr_typed >> 24;

                uint32_t attr_res_id = 0;
                if (res_ids && attr_name_idx < res_id_count) {
                    attr_res_id = res_ids[attr_name_idx];
                }

                const char *attr_name = (attr_name_idx < string_count && strings) ?
                                          strings[attr_name_idx] : "";

                // Detect style="@style/..." attribute.
                // In binary AXML, the style attribute has namespace index 0xFFFFFFFF
                // and name "style", or attr_res_id == 0x010100ba.
                // The value is a resource reference (type 0x01) to the style.
                if (strcmp(attr_name, "style") == 0 || attr_res_id == ATTR_STYLE) {
                    if (attr_type == 0x01) { // resource reference
                        style_res_id = attr_data;
                    }
                    continue; // style is not a view attribute
                }

                // Resolve ?attr/ references (type 0x02) through the theme
                if (attr_type == RES_VALUE_TYPE_ATTR && ctx && ctx->theme) {
                    uint8_t resolved_type = (uint8_t)attr_type;
                    uint32_t resolved_data = attr_data;
                    resolve_attr_reference(ctx, &resolved_type, &resolved_data);
                    if (resolved_type != RES_VALUE_TYPE_ATTR) {
                        attr_type = resolved_type;
                        attr_data = resolved_data;
                    }
                }

                // android:id
                if (attr_res_id == ATTR_ID || strcmp(attr_name, "id") == 0) {
                    node->view_id = attr_data;
                }
                // android:text
                else if (attr_res_id == ATTR_TEXT || strcmp(attr_name, "text") == 0) {
                    if (attr_type == 0x03 && attr_raw < string_count && strings) {
                        // String pool reference
                        dx_ui_node_set_text(node, strings[attr_raw]);
                    } else if (attr_type == 0x01) {
                        // Resource reference - look up string from resources
                        const char *res_text = NULL;
                        if (ctx) {
                            uint32_t entry = attr_data & 0xFFFF;
                            if (entry < ctx->string_resource_count && ctx->string_resources) {
                                res_text = ctx->string_resources[entry];
                            }
                        }
                        if (res_text) {
                            dx_ui_node_set_text(node, res_text);
                        } else {
                            char buf[32];
                            snprintf(buf, sizeof(buf), "@0x%08x", attr_data);
                            dx_ui_node_set_text(node, buf);
                        }
                    }
                }
                // android:orientation
                else if (attr_res_id == ATTR_ORIENTATION || strcmp(attr_name, "orientation") == 0) {
                    node->orientation = (attr_data == 1) ? DX_ORIENTATION_VERTICAL :
                                                            DX_ORIENTATION_HORIZONTAL;
                }
                // android:gravity
                else if (attr_res_id == ATTR_GRAVITY || strcmp(attr_name, "gravity") == 0) {
                    node->gravity = (int32_t)attr_data;
                }
                // android:hint
                else if (attr_res_id == ATTR_HINT || strcmp(attr_name, "hint") == 0) {
                    if (attr_type == 0x03 && attr_raw < string_count && strings) {
                        if (node->hint) { dx_free(node->hint); node->hint = NULL; }
                        node->hint = dx_strdup(strings[attr_raw]);
                    }
                }
                // android:textColor
                else if (attr_res_id == ATTR_TEXT_COLOR || strcmp(attr_name, "textColor") == 0) {
                    if (attr_type == 0x1C || attr_type == 0x1D) { // color int
                        node->text_color = attr_data;
                    }
                }
                // android:textSize
                else if (attr_res_id == ATTR_TEXT_SIZE || strcmp(attr_name, "textSize") == 0) {
                    if (attr_type == 0x05) { // dimension
                        float val = dx_ui_decode_dimension(attr_data);
                        if (val > 0 && val < 200) node->text_size = val;
                    }
                }
                // android:background (color)
                else if (attr_res_id == ATTR_BACKGROUND || strcmp(attr_name, "background") == 0) {
                    if (attr_type == 0x1C || attr_type == 0x1D) {
                        node->bg_color = attr_data;
                    }
                }
                // android:padding (all sides)
                else if (attr_res_id == ATTR_PADDING || strcmp(attr_name, "padding") == 0) {
                    int32_t pts = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                    node->padding[0] = node->padding[1] = node->padding[2] = node->padding[3] = pts;
                }
                // android:paddingLeft
                else if (attr_res_id == ATTR_PADDING_L || strcmp(attr_name, "paddingLeft") == 0) {
                    node->padding[0] = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                }
                // android:paddingTop
                else if (attr_res_id == ATTR_PADDING_T || strcmp(attr_name, "paddingTop") == 0) {
                    node->padding[1] = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                }
                // android:paddingRight
                else if (attr_res_id == ATTR_PADDING_R || strcmp(attr_name, "paddingRight") == 0) {
                    node->padding[2] = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                }
                // android:paddingBottom
                else if (attr_res_id == ATTR_PADDING_B || strcmp(attr_name, "paddingBottom") == 0) {
                    node->padding[3] = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                }
                // android:paddingStart (maps to left in LTR)
                else if (attr_res_id == ATTR_PADDING_START || strcmp(attr_name, "paddingStart") == 0) {
                    node->padding[0] = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                }
                // android:paddingEnd (maps to right in LTR)
                else if (attr_res_id == ATTR_PADDING_END || strcmp(attr_name, "paddingEnd") == 0) {
                    node->padding[2] = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                }
                // android:visibility
                else if (attr_res_id == ATTR_VISIBILITY || strcmp(attr_name, "visibility") == 0) {
                    if (attr_data == 0) node->visibility = DX_VISIBLE;
                    else if (attr_data == 4) node->visibility = DX_INVISIBLE;
                    else if (attr_data == 8) node->visibility = DX_GONE;
                }
                // android:checked
                else if (attr_res_id == ATTR_CHECKED || strcmp(attr_name, "checked") == 0) {
                    node->is_checked = (attr_data != 0);
                }
                // android:layout_width
                else if (attr_res_id == ATTR_LAYOUT_WIDTH || strcmp(attr_name, "layout_width") == 0) {
                    if (attr_type == 0x05) {
                        // Explicit dimension value (e.g., 200dp)
                        node->width = dx_ui_decode_dimension_int(attr_data);
                    } else {
                        // Special values: -1 match_parent, -2 wrap_content, or raw int
                        node->width = (int32_t)attr_data;
                    }
                }
                // android:layout_height
                else if (attr_res_id == ATTR_LAYOUT_HEIGHT || strcmp(attr_name, "layout_height") == 0) {
                    if (attr_type == 0x05) {
                        // Explicit dimension value (e.g., 48dp)
                        node->height = dx_ui_decode_dimension_int(attr_data);
                    } else {
                        // Special values: -1 match_parent, -2 wrap_content, or raw int
                        node->height = (int32_t)attr_data;
                    }
                }
                // android:layout_weight
                else if (attr_res_id == ATTR_LAYOUT_WEIGHT || strcmp(attr_name, "layout_weight") == 0) {
                    // attr_type 0x04 = float
                    if (attr_type == 0x04) {
                        // Reinterpret attr_data bits as float
                        union { uint32_t u; float f; } conv;
                        conv.u = attr_data;
                        node->weight = conv.f;
                    } else {
                        node->weight = (float)attr_data;
                    }
                }
                // android:layout_margin (all sides)
                else if (attr_res_id == ATTR_LAYOUT_MARGIN || strcmp(attr_name, "layout_margin") == 0) {
                    int32_t pts = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                    node->margin[0] = node->margin[1] = node->margin[2] = node->margin[3] = pts;
                }
                // android:layout_marginLeft
                else if (attr_res_id == ATTR_LAYOUT_MARGIN_L || strcmp(attr_name, "layout_marginLeft") == 0) {
                    node->margin[0] = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                }
                // android:layout_marginTop
                else if (attr_res_id == ATTR_LAYOUT_MARGIN_T || strcmp(attr_name, "layout_marginTop") == 0) {
                    node->margin[1] = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                }
                // android:layout_marginRight
                else if (attr_res_id == ATTR_LAYOUT_MARGIN_R || strcmp(attr_name, "layout_marginRight") == 0) {
                    node->margin[2] = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                }
                // android:layout_marginBottom
                else if (attr_res_id == ATTR_LAYOUT_MARGIN_B || strcmp(attr_name, "layout_marginBottom") == 0) {
                    node->margin[3] = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                }
                // android:layout_marginStart (maps to left in LTR)
                else if (attr_res_id == ATTR_LAYOUT_MARGIN_START || strcmp(attr_name, "layout_marginStart") == 0) {
                    node->margin[0] = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                }
                // android:layout_marginEnd (maps to right in LTR)
                else if (attr_res_id == ATTR_LAYOUT_MARGIN_END || strcmp(attr_name, "layout_marginEnd") == 0) {
                    node->margin[2] = (attr_type == 0x05) ? dx_ui_decode_dimension_int(attr_data) : (int32_t)attr_data;
                }
                // android:src (ImageView drawable resource)
                else if (attr_res_id == ATTR_SRC || strcmp(attr_name, "src") == 0 ||
                         strcmp(attr_name, "srcCompat") == 0) {
                    if (attr_type == 0x01 && ctx) {
                        // Resource reference - extract drawable image from APK
                        uint32_t img_len = 0;
                        uint8_t *img_data = dx_ui_extract_drawable(ctx, attr_data, &img_len);
                        if (img_data && img_len > 0) {
                            node->image_data = img_data;
                            node->image_data_len = img_len;
                        }
                    }
                }
                // RelativeLayout: parent alignment flags (boolean attrs: 0x12 = true/-1)
                else if (attr_res_id == ATTR_LAYOUT_ALIGN_PARENT_TOP ||
                         strcmp(attr_name, "layout_alignParentTop") == 0) {
                    if (attr_data != 0) node->relative_flags |= DX_REL_ALIGN_PARENT_TOP;
                }
                else if (attr_res_id == ATTR_LAYOUT_ALIGN_PARENT_BOTTOM ||
                         strcmp(attr_name, "layout_alignParentBottom") == 0) {
                    if (attr_data != 0) node->relative_flags |= DX_REL_ALIGN_PARENT_BOTTOM;
                }
                else if (attr_res_id == ATTR_LAYOUT_ALIGN_PARENT_LEFT ||
                         strcmp(attr_name, "layout_alignParentLeft") == 0) {
                    if (attr_data != 0) node->relative_flags |= DX_REL_ALIGN_PARENT_LEFT;
                }
                else if (attr_res_id == ATTR_LAYOUT_ALIGN_PARENT_RIGHT ||
                         strcmp(attr_name, "layout_alignParentRight") == 0) {
                    if (attr_data != 0) node->relative_flags |= DX_REL_ALIGN_PARENT_RIGHT;
                }
                else if (attr_res_id == ATTR_LAYOUT_CENTER_IN_PARENT ||
                         strcmp(attr_name, "layout_centerInParent") == 0) {
                    if (attr_data != 0) node->relative_flags |= DX_REL_CENTER_IN_PARENT;
                }
                else if (attr_res_id == ATTR_LAYOUT_CENTER_HORIZONTAL ||
                         strcmp(attr_name, "layout_centerHorizontal") == 0) {
                    if (attr_data != 0) node->relative_flags |= DX_REL_CENTER_HORIZONTAL;
                }
                else if (attr_res_id == ATTR_LAYOUT_CENTER_VERTICAL ||
                         strcmp(attr_name, "layout_centerVertical") == 0) {
                    if (attr_data != 0) node->relative_flags |= DX_REL_CENTER_VERTICAL;
                }
                // RelativeLayout: sibling reference attrs (value = view ID)
                else if (attr_res_id == ATTR_LAYOUT_ABOVE ||
                         strcmp(attr_name, "layout_above") == 0) {
                    node->relative_flags |= DX_REL_ABOVE;
                    node->rel_above = attr_data;
                }
                else if (attr_res_id == ATTR_LAYOUT_BELOW ||
                         strcmp(attr_name, "layout_below") == 0) {
                    node->relative_flags |= DX_REL_BELOW;
                    node->rel_below = attr_data;
                }
                else if (attr_res_id == ATTR_LAYOUT_TO_LEFT_OF ||
                         strcmp(attr_name, "layout_toLeftOf") == 0) {
                    node->relative_flags |= DX_REL_LEFT_OF;
                    node->rel_left_of = attr_data;
                }
                else if (attr_res_id == ATTR_LAYOUT_TO_RIGHT_OF ||
                         strcmp(attr_name, "layout_toRightOf") == 0) {
                    node->relative_flags |= DX_REL_RIGHT_OF;
                    node->rel_right_of = attr_data;
                }
                // ConstraintLayout constraint attributes (app: namespace, matched by name)
                // Value is either a view ID (resource reference) or "parent" (string 0x03 → look up)
                // For resource refs (type 0x01), attr_data is the view ID.
                // For "parent" string, we use DX_CONSTRAINT_PARENT sentinel.
                else if (strcmp(attr_name, "layout_constraintLeft_toLeftOf") == 0 ||
                         strcmp(attr_name, "layout_constraintStart_toStartOf") == 0) {
                    if (attr_type == 0x01) {
                        node->constraints.left_to_left = attr_data;
                    } else if (attr_type == 0x03 && attr_raw < string_count && strings &&
                               strcmp(strings[attr_raw], "parent") == 0) {
                        node->constraints.left_to_left = DX_CONSTRAINT_PARENT;
                    } else {
                        // Integer 0 often means "parent" in compiled AXML
                        node->constraints.left_to_left = (attr_data == 0) ? DX_CONSTRAINT_PARENT : attr_data;
                    }
                }
                else if (strcmp(attr_name, "layout_constraintRight_toRightOf") == 0 ||
                         strcmp(attr_name, "layout_constraintEnd_toEndOf") == 0) {
                    if (attr_type == 0x01) {
                        node->constraints.right_to_right = attr_data;
                    } else if (attr_type == 0x03 && attr_raw < string_count && strings &&
                               strcmp(strings[attr_raw], "parent") == 0) {
                        node->constraints.right_to_right = DX_CONSTRAINT_PARENT;
                    } else {
                        node->constraints.right_to_right = (attr_data == 0) ? DX_CONSTRAINT_PARENT : attr_data;
                    }
                }
                else if (strcmp(attr_name, "layout_constraintTop_toTopOf") == 0) {
                    if (attr_type == 0x01) {
                        node->constraints.top_to_top = attr_data;
                    } else if (attr_type == 0x03 && attr_raw < string_count && strings &&
                               strcmp(strings[attr_raw], "parent") == 0) {
                        node->constraints.top_to_top = DX_CONSTRAINT_PARENT;
                    } else {
                        node->constraints.top_to_top = (attr_data == 0) ? DX_CONSTRAINT_PARENT : attr_data;
                    }
                }
                else if (strcmp(attr_name, "layout_constraintBottom_toBottomOf") == 0) {
                    if (attr_type == 0x01) {
                        node->constraints.bottom_to_bottom = attr_data;
                    } else if (attr_type == 0x03 && attr_raw < string_count && strings &&
                               strcmp(strings[attr_raw], "parent") == 0) {
                        node->constraints.bottom_to_bottom = DX_CONSTRAINT_PARENT;
                    } else {
                        node->constraints.bottom_to_bottom = (attr_data == 0) ? DX_CONSTRAINT_PARENT : attr_data;
                    }
                }
                else if (strcmp(attr_name, "layout_constraintTop_toBottomOf") == 0) {
                    if (attr_type == 0x01) {
                        node->constraints.top_to_bottom = attr_data;
                    } else if (attr_type == 0x03 && attr_raw < string_count && strings &&
                               strcmp(strings[attr_raw], "parent") == 0) {
                        node->constraints.top_to_bottom = DX_CONSTRAINT_PARENT;
                    } else {
                        node->constraints.top_to_bottom = (attr_data == 0) ? DX_CONSTRAINT_PARENT : attr_data;
                    }
                }
                else if (strcmp(attr_name, "layout_constraintBottom_toTopOf") == 0) {
                    if (attr_type == 0x01) {
                        node->constraints.bottom_to_top = attr_data;
                    } else if (attr_type == 0x03 && attr_raw < string_count && strings &&
                               strcmp(strings[attr_raw], "parent") == 0) {
                        node->constraints.bottom_to_top = DX_CONSTRAINT_PARENT;
                    } else {
                        node->constraints.bottom_to_top = (attr_data == 0) ? DX_CONSTRAINT_PARENT : attr_data;
                    }
                }
                else if (strcmp(attr_name, "layout_constraintLeft_toRightOf") == 0 ||
                         strcmp(attr_name, "layout_constraintStart_toEndOf") == 0) {
                    if (attr_type == 0x01) {
                        node->constraints.left_to_right = attr_data;
                    } else if (attr_type == 0x03 && attr_raw < string_count && strings &&
                               strcmp(strings[attr_raw], "parent") == 0) {
                        node->constraints.left_to_right = DX_CONSTRAINT_PARENT;
                    } else {
                        node->constraints.left_to_right = (attr_data == 0) ? DX_CONSTRAINT_PARENT : attr_data;
                    }
                }
                else if (strcmp(attr_name, "layout_constraintRight_toLeftOf") == 0 ||
                         strcmp(attr_name, "layout_constraintEnd_toStartOf") == 0) {
                    if (attr_type == 0x01) {
                        node->constraints.right_to_left = attr_data;
                    } else if (attr_type == 0x03 && attr_raw < string_count && strings &&
                               strcmp(strings[attr_raw], "parent") == 0) {
                        node->constraints.right_to_left = DX_CONSTRAINT_PARENT;
                    } else {
                        node->constraints.right_to_left = (attr_data == 0) ? DX_CONSTRAINT_PARENT : attr_data;
                    }
                }
                else if (strcmp(attr_name, "layout_constraintHorizontal_bias") == 0) {
                    if (attr_type == 0x04) {
                        union { uint32_t u; float f; } conv;
                        conv.u = attr_data;
                        node->constraints.horizontal_bias = conv.f;
                    }
                }
                else if (strcmp(attr_name, "layout_constraintVertical_bias") == 0) {
                    if (attr_type == 0x04) {
                        union { uint32_t u; float f; } conv;
                        conv.u = attr_data;
                        node->constraints.vertical_bias = conv.f;
                    }
                }
            }

            // Apply style attributes as defaults (direct attrs take precedence)
            if (style_res_id != 0 && ctx && ctx->resources) {
                DxStyleBag *style = dx_resources_resolve_style(ctx->resources, style_res_id);
                if (style) {
                    apply_style_to_node(node, ctx, style, strings, string_count);
                    dx_style_bag_free(style);
                    DX_TRACE(TAG, "Applied style 0x%08x to <%s>", style_res_id, tag);
                }
            }

            // Add to tree
            if (stack_depth > 0) {
                dx_ui_node_add_child(stack[stack_depth - 1], node);
            } else {
                root = node;
            }

            if (stack_depth < AXML_MAX_NESTING_DEPTH) {
                stack[stack_depth++] = node;
            } else {
                DX_WARN(TAG, "AXML nesting depth exceeds %d levels", AXML_MAX_NESTING_DEPTH);
                // Clean up and bail out
                for (uint32_t si = 0; si < string_count; si++) {
                    dx_free(strings[si]);
                }
                dx_free(strings);
                dx_free(res_ids);
                if (root) dx_ui_node_destroy(root);
                return DX_ERR_AXML_INVALID;
            }

            DX_DEBUG(TAG, "Layout: <%s id=0x%x text=\"%s\">",
                     tag, node->view_id, node->text ? node->text : "");

        } else if (chunk_type == AXML_CHUNK_END_TAG) {
            if (stack_depth > 0) stack_depth--;
        }

        pos += chunk_size;
    }

    // Cleanup
    for (uint32_t i = 0; i < string_count; i++) {
        dx_free(strings[i]);
    }
    dx_free(strings);
    dx_free(res_ids);

    if (root) {
        DX_INFO(TAG, "Layout parsed: root type=%d, %u children",
                root->type, root->child_count);
        *out = root;
        return DX_OK;
    }

    return DX_ERR_AXML_INVALID;
}
