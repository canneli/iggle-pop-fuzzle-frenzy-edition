#define COBJMACROS
#include <windows.h>
#include <msxml2.h>
#include <wchar.h>
#include <string.h>
#include <stdio.h>
#include "iggle_level_editor.h"
#include "iggle_mods.h"

static IXMLDOMDocument2 *document;
static EditorBoard board;
static SRWLOCK board_lock = SRWLOCK_INIT;
static BOOL com_initialized, dirty;
static const wchar_t *objects[] = { L"playerSpawner", L"cageSpawner", L"fuzzleHome", L"monsterSpawner" };
static const wchar_t *colors[] = { L"Green", L"Blue", L"Yellow", L"Purple" };
static const wchar_t *hero_keys[]={L"Fizzy",L"Dizzy",L"Bozz",L"Rizzo",L"Doc"};
static const char *hero_names[]={"Fizzy","Dizzy","Bozz","Rizzo","Doc"};
const char *iggle_editor_hero_name(int hero){return hero_names[hero>=0&&hero<EDIT_HERO_COUNT?hero:0];}
static int read_hero(IXMLDOMDocument2 *doc)
{
    IXMLDOMElement *root=NULL;IXMLDOMDocument2_get_documentElement(doc,&root);
    if(!root)return 0;
    VARIANT value;VariantInit(&value);BSTR key=SysAllocString(L"hdHero");
    root->lpVtbl->getAttribute(root,key,&value);SysFreeString(key);root->lpVtbl->Release(root);
    int hero=0;if(value.vt==VT_BSTR&&value.bstrVal)for(int i=0;i<EDIT_HERO_COUNT;++i)if(!_wcsicmp(value.bstrVal,hero_keys[i]))hero=i;
    VariantClear(&value);return hero;
}
static const wchar_t *const monster_types[EDIT_ZOOG_COUNT] = {
    L"Basic Tracker",L"Med Tracker",L"Basic Blobby",L"Med Blobby",L"BasicShooter",L"Armor Zoog",
    L"Fire Zoog",L"Tunnel Zoog",L"Super Zoog",L"Guard",L"Guard Fast",L"Bomber-SlwAcc",
    L"Bomber-SlwRnd",L"Bomber-FstAcc",L"Bomber-FstRnd",L"Bomber-Hyper",L"GMZ",L"HD Dragon"
};
static const wchar_t *const power_tags[EDIT_POWER_COUNT] = {L"freezePickup",L"zoogZapper",L"autoSort",L"fuzzlePop",L"freezePickup"};
static const wchar_t *const world_keys[] = {L"editorTheme.xml", L"snowTheme.xml", L"desertTheme.xml", L"caveTheme.xml", L"shipTheme.xml"};
static const char *const theme_paths[] = {"res/Themes/editorTheme.xml", "res/Themes/snowTheme.xml",
    "res/Themes/desertTheme.xml", "res/Themes/caveTheme.xml", "res/Themes/shipTheme.xml"};

static const wchar_t blank_level[] =
    L"<level editorTheme=\"res\\Themes\\editorTheme.xml\" name=\"\" type=\"normal\" time=\"999\" "
    L"maxLetterCount=\"1\" letterDelayRange=\"(25.000000,45.000000)\" musicOverload=\"\">"
    L"<maze gridSize=\"11,10\" cellSize=\"47\" gridOffset=\"118,9\"/>"
    L"<objectList><playerSpawner cellLoc=\"5,4\"/><cageSpawner cellLoc=\"6,4\" type=\"0\" color=\"Green\"/>"
    L"<fuzzleHome cellLoc=\"8,4\" color=\"Green\" minCount=\"0\"/></objectList><decoList/></level>";

static IXMLDOMNode *find_node(const wchar_t *query)
{
    IXMLDOMNode *node = NULL;
    BSTR argument = SysAllocString(query);
    IXMLDOMDocument2_selectSingleNode(document, argument, &node);
    SysFreeString(argument);
    return node;
}

static BSTR read_attribute(IXMLDOMNode *node, const wchar_t *attribute)
{
    IXMLDOMNode *value = NULL;
    BSTR name = SysAllocString(attribute), text = NULL;
    node->lpVtbl->selectSingleNode(node, name, &value);
    SysFreeString(name);
    if (value) {
        value->lpVtbl->get_text(value, &text);
        value->lpVtbl->Release(value);
    }
    return text;
}

static BOOL coordinates(IXMLDOMNode *node, const wchar_t *attribute, int *x, int *y)
{
    BSTR text = read_attribute(node, attribute);
    wchar_t extra;
    BOOL valid = text && wcslen(text) <= 12 && swscanf(text, L"%d,%d%c", x, y, &extra) == 2;
    SysFreeString(text);
    return valid;
}

static int read_world(IXMLDOMDocument2 *doc)
{
    IXMLDOMElement *root = NULL;
    IXMLDOMDocument2_get_documentElement(doc, &root);
    if (!root) return 0;
    BSTR path = read_attribute((IXMLDOMNode *)root,L"@editorTheme");
    root->lpVtbl->Release(root);
    if (!path) return 0;
    const wchar_t *name = path;
    for (const wchar_t *c = path; *c; ++c)
        if (*c == L'/' || *c == L'\\') name = c+1;
    int world = 0;
    char id[IGGLE_MOD_NAME];
    if (iggle_mods_world(id,NULL)) {
        wchar_t key[66];
        swprintf(key,ARRAYSIZE(key),L"igglemod-%hs.xml",id);
        if (!_wcsicmp(name,key)) world=EDIT_WORLD_COUNT;
    }
    for (int i = 1; i < EDIT_WORLD_COUNT; ++i)
        if (!_wcsicmp(world_keys[i], name)) world = i;
    SysFreeString(path);
    return world;
}

static void read_tiles(EditorBoard *next, const wchar_t *query, const wchar_t *attr, unsigned char id, BOOL object, int variant)
{
    IXMLDOMNodeList *nodes = NULL;
    BSTR argument = SysAllocString(query);
    IXMLDOMDocument2_selectNodes(document, argument, &nodes);
    SysFreeString(argument);
    if (!nodes) return;
    IXMLDOMNode *node;
    while (nodes->lpVtbl->nextNode(nodes, &node) == S_OK && node) {
        int x, y;
        if (coordinates(node, attr, &x, &y) && x >= 0 && y >= 0 &&
            x < next->columns + !object && y < next->rows + !object) {
            if (object) {
                if (id == EDIT_POWER) {
                    BSTR restored = read_attribute(node,L"@hdRestored");
                    BOOL slow = restored && !wcscmp(restored,L"Slow");
                    if (slow ? variant==POWER_SLOW : variant!=POWER_SLOW)
                        next->powers[y][x] = variant+1;
                    SysFreeString(restored);
                }
                else next->objects[y][x] = id;
                if (id == EDIT_ZOOG) {
                    BSTR type = read_attribute(node,L"@type");
                    if (type) for (int i=0;i<EDIT_ZOOG_COUNT;++i)
                        if (!_wcsicmp(type,monster_types[i])) { next->monsters[y][x] = i; break; }
                    BSTR restored = read_attribute(node,L"@hdRestored");
                    if (restored && !wcscmp(restored,L"Dragon")) next->monsters[y][x]=ZOOG_DRAGON;
                    SysFreeString(restored);
                    SysFreeString(type);
                }
                if (id == EDIT_CAGE || id == EDIT_HOME || (id==EDIT_POWER && variant==POWER_POP)) {
                    BSTR color = read_attribute(node,L"@color");
                    unsigned char *cell = id==EDIT_POWER ? &next->power_colors[y][x] : &next->colors[y][x];
                    *cell = 0;
                    if (color) for (int i=1;i<EDIT_COLOR_COUNT;++i)
                        if (!_wcsicmp(color,colors[i])) *cell = i;
                    SysFreeString(color);
                    if (id==EDIT_CAGE) {
                        BSTR restored = read_attribute(node,L"@hdRestored");
                        if (restored && !wcscmp(restored,L"Rainbow")) *cell=EDIT_RAINBOW;
                        SysFreeString(restored);
                    }
                }
            }
            else next->tiles[y][x] |= id;
        }
        node->lpVtbl->Release(node);
    }
    nodes->lpVtbl->Release(nodes);
}

static BOOL read_board(void)
{
    EditorBoard next = {0};
    IXMLDOMNode *maze = find_node(L"/level/maze");
    if (!maze) return FALSE;
    BOOL okay = coordinates(maze, L"@gridSize", &next.columns, &next.rows);
    maze->lpVtbl->Release(maze);
    if (!okay || next.columns < 1 || next.rows < 1 ||
        next.columns > EDIT_GRID_MAX || next.rows > EDIT_GRID_MAX) return FALSE;

    next.world = read_world(document);
    next.hero = read_hero(document);
    read_tiles(&next, L"/level/maze/floor", L"@cell", EDIT_HAS_FLOOR, FALSE, 0);
    read_tiles(&next, L"/level/maze/edge[@dir='h']", L"@edgeIndex", EDIT_HAS_WALL_H, FALSE, 0);
    read_tiles(&next, L"/level/maze/edge[@dir='v']", L"@edgeIndex", EDIT_HAS_WALL_V, FALSE, 0);
    for (int i = 0; i < ARRAYSIZE(objects); ++i) {
        wchar_t query[60];
        swprintf(query, ARRAYSIZE(query), L"/level/objectList/%ls", objects[i]);
        read_tiles(&next, query, L"@cellLoc", EDIT_HERO+i, TRUE, 0);
    }
    for (int i = 0; i < EDIT_POWER_COUNT; ++i) {
        wchar_t query[60];
        swprintf(query, ARRAYSIZE(query), L"/level/objectList/%ls", power_tags[i]);
        read_tiles(&next, query, L"@cellLoc", EDIT_POWER, TRUE, i);
    }
    AcquireSRWLockExclusive(&board_lock);
    board = next;
    ReleaseSRWLockExclusive(&board_lock);
    return TRUE;
}

void iggle_editor_board(EditorBoard *copy)
{
    AcquireSRWLockShared(&board_lock);
    *copy = board;
    ReleaseSRWLockShared(&board_lock);
}

void iggle_editor_close(void)
{
    if (document) IXMLDOMDocument2_Release(document);
    document = NULL;
    if (com_initialized) CoUninitialize();
    com_initialized = FALSE;
    dirty = FALSE;
}

static IXMLDOMDocument2 *create_document(void)
{
    // MSXML 6 rejects DTDs by default. Keep external resolution disabled.
    static const CLSID dom6 = {0x88d96a05, 0xf192, 0x11d4, {0xa6,0x5f,0,0x40,0x96,0x32,0x51,0xe5}};
    static const IID document2 = {0x2933bf95, 0x7b36, 0x11d2, {0xb2,0x0e,0,0xc0,0x4f,0x98,0x3e,0x60}};
    IXMLDOMDocument2 *doc = NULL;
    if (FAILED(CoCreateInstance(&dom6, NULL, CLSCTX_INPROC_SERVER, &document2, (void **)&doc))) return NULL;
    IXMLDOMDocument2_put_async(doc, VARIANT_FALSE);
    IXMLDOMDocument2_put_validateOnParse(doc, VARIANT_FALSE);
    IXMLDOMDocument2_put_resolveExternals(doc, VARIANT_FALSE);

    return doc;
}

static BOOL load_document(IXMLDOMDocument2 *doc,const wchar_t *path)
{
    VARIANT_BOOL loaded = VARIANT_FALSE;
    VARIANT source = { .vt=VT_BSTR, .bstrVal=SysAllocString(path) };
    IXMLDOMDocument2_load(doc,source,&loaded);
    VariantClear(&source);
    return loaded == VARIANT_TRUE;
}

static BOOL remove_node(IXMLDOMNode *node);
static BOOL write_level_document(IXMLDOMDocument2 *doc, const wchar_t *path);

BOOL iggle_level_prepare(char path[MAX_PATH], char temporary[MAX_PATH])
{
    wchar_t filename[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, filename, ARRAYSIZE(filename))) return FALSE;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return FALSE;

    IXMLDOMDocument2 *doc = create_document();
    BOOL valid = FALSE, filtered = FALSE;
    if (doc && load_document(doc, filename)) {
        IXMLDOMNode *maze = NULL;
        BSTR query = SysAllocString(L"/level/maze");
        IXMLDOMDocument2_selectSingleNode(doc, query, &maze);
        SysFreeString(query);
        int columns, rows;
        if (maze) {
            valid = coordinates(maze, L"@gridSize", &columns, &rows) &&
                columns > 0 && rows > 0 && columns <= EDIT_GRID_MAX && rows <= EDIT_GRID_MAX;
            maze->lpVtbl->Release(maze);
        }

        if (valid) {
            IXMLDOMNodeList *edges = NULL;
            query = SysAllocString(L"/level/maze/edge");
            IXMLDOMDocument2_selectNodes(doc, query, &edges);
            SysFreeString(query);
            // The retail loader stores both edge sets in arrays sized to the grid.
            // Walk the snapshot backwards as the outer edges are removed.
            long count = 0;
            if (edges) edges->lpVtbl->get_length(edges, &count);
            for (long i = count; i > 0 && valid; --i) {
                IXMLDOMNode *edge = NULL;
                if (edges->lpVtbl->get_item(edges, i - 1, &edge) != S_OK || !edge) {
                    valid = FALSE;
                    break;
                }
                int x, y;
                if (!coordinates(edge, L"@edgeIndex", &x, &y) ||
                    x < 0 || y < 0 || x >= columns || y >= rows) {
                    valid = remove_node(edge);
                    filtered = TRUE;
                }
                edge->lpVtbl->Release(edge);
            }
            if (edges) edges->lpVtbl->Release(edges);
        }
    }

    if (valid && filtered) {
        wchar_t folder[MAX_PATH], staged[MAX_PATH];
        DWORD length = GetTempPathW(ARRAYSIZE(folder), folder);
        valid = length && length < ARRAYSIZE(folder) && GetTempFileNameW(folder, L"igl", 0, staged);
        if (valid) {
            BOOL substituted = FALSE;
            UINT code_page = GetACP();
            valid = WideCharToMultiByte(code_page, 0, staged, -1, temporary, MAX_PATH, NULL,
                    code_page == CP_UTF8 ? NULL : &substituted) > 0 &&
                !substituted && write_level_document(doc, staged);
            if (valid) strcpy(path, temporary);
            else DeleteFileW(staged);
        }
    }
    if (!valid) temporary[0] = 0;
    if (doc) IXMLDOMDocument2_Release(doc);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return valid;
}

int iggle_level_hero(const char *path)
{
    wchar_t filename[MAX_PATH];
    if(!path||!MultiByteToWideChar(CP_ACP,0,path,-1,filename,ARRAYSIZE(filename)))return 0;
    HRESULT result=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    if(FAILED(result)&&result!=RPC_E_CHANGED_MODE)return 0;
    IXMLDOMDocument2 *doc=create_document();int hero=0;
    if(doc){if(load_document(doc,filename))hero=read_hero(doc);IXMLDOMDocument2_Release(doc);}
    if(SUCCEEDED(result))CoUninitialize();return hero;
}

const char *iggle_level_theme(const char *path)
{
    wchar_t filename[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP,0,path,-1,filename,ARRAYSIZE(filename))) return theme_paths[0];
    HRESULT result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) return theme_paths[0];
    IXMLDOMDocument2 *doc = create_document();
    int world = 0;
    if (doc) {
        if (load_document(doc,filename)) world = read_world(doc);
        IXMLDOMDocument2_Release(doc);
    }
    if (SUCCEEDED(result)) CoUninitialize();
    static char mod_theme[96];
    char id[IGGLE_MOD_NAME];
    if (world==EDIT_WORLD_COUNT && iggle_mods_world(id,NULL)) {
        snprintf(mod_theme,sizeof mod_theme,"res/igglemods/world/%s/theme.xml",id);
        return mod_theme;
    }
    return theme_paths[world<EDIT_WORLD_COUNT?world:0];
}

BOOL iggle_editor_open(const wchar_t *path)
{
    iggle_editor_close();
    HRESULT result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    com_initialized = SUCCEEDED(result);
    if (!com_initialized && result != RPC_E_CHANGED_MODE) return FALSE;

    document = create_document();
    if (!document) { iggle_editor_close(); return FALSE; }

    VARIANT_BOOL loaded = VARIANT_FALSE;
    if (path) {
        loaded = load_document(document,path) ? VARIANT_TRUE : VARIANT_FALSE;
    } else {
        BSTR source = SysAllocString(blank_level);
        IXMLDOMDocument2_loadXML(document, source, &loaded);
        SysFreeString(source);
    }
    if (loaded && read_board()) return TRUE;
    iggle_editor_close();
    return FALSE;
}

BOOL iggle_editor_dirty(void) { return dirty; }

static void attribute(IXMLDOMElement *node, const wchar_t *key, const wchar_t *text);

static void resolve_rainbow_homes(void)
{
    IXMLDOMNodeList *cages = NULL;
    BSTR query = SysAllocString(L"/level/objectList/cageSpawner[@hdRestored='Rainbow']");
    IXMLDOMDocument2_selectNodes(document, query, &cages);
    SysFreeString(query);
    if (!cages) return;
    long count = 0;
    cages->lpVtbl->get_length(cages, &count);
    for (long i=0; i<count; i++) {
        IXMLDOMNode *cage = NULL;
        if (cages->lpVtbl->get_item(cages, i, &cage) != S_OK || !cage) continue;
        int x, y, closest = EDIT_GRID_MAX*2, color = 0;
        if (coordinates(cage, L"@cellLoc", &x, &y) &&
            x >= 0 && x < board.columns && y >= 0 && y < board.rows) {
            for (int row=0; row<board.rows; row++) {
                for (int column=0; column<board.columns; column++) {
                    if (board.objects[row][column] != EDIT_HOME) continue;
                    int distance = abs(row-y)+abs(column-x);
                    if (distance >= closest) continue;
                    closest = distance;
                    color = board.colors[row][column];
                }
            }
            IXMLDOMNode *color_attribute = NULL;
            BSTR key = SysAllocString(L"@color");
            cage->lpVtbl->selectSingleNode(cage, key, &color_attribute);
            SysFreeString(key);
            if (color_attribute) {
                BSTR text = SysAllocString(colors[color]);
                color_attribute->lpVtbl->put_text(color_attribute, text);
                SysFreeString(text);
                color_attribute->lpVtbl->Release(color_attribute);
            }
        }
        cage->lpVtbl->Release(cage);
    }
    cages->lpVtbl->Release(cages);
}

static BOOL write_level_document(IXMLDOMDocument2 *doc, const wchar_t *path)
{
    // Write root-only UTF-8, matching the game's XML reader and standalone editor.
    IXMLDOMElement *root = NULL;
    BSTR xml = NULL;
    if (SUCCEEDED(IXMLDOMDocument2_get_documentElement(doc, &root)) && root) {
        root->lpVtbl->get_xml(root, &xml);
        root->lpVtbl->Release(root);
    }
    int size = xml ? WideCharToMultiByte(CP_UTF8, 0, xml, -1, NULL, 0, NULL, NULL) : 0;
    char *bytes = size > 1 ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;
    BOOL saved = FALSE;
    if (bytes && WideCharToMultiByte(CP_UTF8, 0, xml, -1, bytes, size, NULL, NULL)) {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            saved = WriteFile(file, bytes, size-1, &written, NULL) && written == (DWORD)(size-1);
            CloseHandle(file);
        }
    }
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    SysFreeString(xml);
    return saved;
}

BOOL iggle_editor_save(const wchar_t *path)
{
    if (!document) return FALSE;
    resolve_rainbow_homes();
    wchar_t temporary[MAX_PATH];
    wchar_t *separator = wcsrchr(path, L'\\');
    if (!separator || separator-path >= MAX_PATH) return FALSE;
    wchar_t directory[MAX_PATH];
    wmemcpy(directory, path, separator-path);
    directory[separator-path] = 0;
    if (!GetTempFileNameW(directory, L"igg", 0, temporary)) return FALSE;
    BOOL saved = write_level_document(document, temporary);
    if (saved) saved = MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!saved) DeleteFileW(temporary);
    if (saved) dirty = FALSE;
    return saved;
}

static BOOL remove_node(IXMLDOMNode *node)
{
    IXMLDOMNode *parent = NULL, *removed = NULL;
    node->lpVtbl->get_parentNode(node, &parent);
    if (!parent) return FALSE;
    HRESULT result = parent->lpVtbl->removeChild(parent, node, &removed);
    if (removed) removed->lpVtbl->Release(removed);
    parent->lpVtbl->Release(parent);
    return SUCCEEDED(result);
}

static void attribute(IXMLDOMElement *node, const wchar_t *key, const wchar_t *text)
{
    BSTR name = SysAllocString(key);
    VARIANT value = { .vt = VT_BSTR, .bstrVal = SysAllocString(text) };
    node->lpVtbl->setAttribute(node, name, value);
    VariantClear(&value);
    SysFreeString(name);
}

BOOL iggle_editor_set_hero(int hero)
{
    if(!document||hero<0||hero>=EDIT_HERO_COUNT)return FALSE;
    IXMLDOMElement *root=NULL;IXMLDOMDocument2_get_documentElement(document,&root);
    if(!root)return FALSE;
    attribute(root,L"hdHero",hero_keys[hero]);root->lpVtbl->Release(root);
    dirty=TRUE;return read_board();
}

BOOL iggle_editor_set_world(int world)
{
    if (!document || world < 0 || world > EDIT_WORLD_COUNT) return FALSE;
    char id[IGGLE_MOD_NAME];
    if (world==EDIT_WORLD_COUNT && !iggle_mods_world(id,NULL)) return FALSE;
    IXMLDOMElement *root = NULL;
    IXMLDOMDocument2_get_documentElement(document, &root);
    if (!root) return FALSE;
    wchar_t path[80];
    if (world==EDIT_WORLD_COUNT) swprintf(path,ARRAYSIZE(path),L"res\\Themes\\igglemod-%hs.xml",id);
    else swprintf(path,ARRAYSIZE(path),L"res\\Themes\\%ls",world_keys[world]);
    attribute(root,L"editorTheme",path);
    root->lpVtbl->Release(root);
    dirty = TRUE;
    return read_board();
}

static IXMLDOMElement *new_element(const wchar_t *tag)
{
    IXMLDOMElement *item = NULL;
    BSTR name = SysAllocString(tag);
    IXMLDOMDocument2_createElement(document, name, &item);
    SysFreeString(name);
    return item;
}

static BOOL append(IXMLDOMNode *parent, IXMLDOMElement *child)
{
    if (!parent || !child) return FALSE;
    IXMLDOMNode *added = NULL;
    HRESULT result = parent->lpVtbl->appendChild(parent, (IXMLDOMNode *)child, &added);
    if (added) added->lpVtbl->Release(added);
    return SUCCEEDED(result);
}

BOOL iggle_editor_paint(int x, int y, int side, int tool, int color, int variant)
{
    if (!document || x < 0 || y < 0 || x >= board.columns || y >= board.rows ||
        tool < 0 || tool >= EDIT_TOOL_COUNT || color < 0 || color > EDIT_RAINBOW || (color==EDIT_RAINBOW && tool!=EDIT_CAGE)) return FALSE;
    if ((tool==EDIT_ZOOG && (variant<0 || variant>=EDIT_ZOOG_COUNT)) ||
        (tool==EDIT_POWER && (variant<0 || variant>=EDIT_POWER_COUNT))) return FALSE;
    if (tool == EDIT_WALL && ((side == 3 && x + 1 == board.columns) ||
                             (side == 2 && y + 1 == board.rows))) return FALSE;
    wchar_t cell[24], edge[24], query[170];
    swprintf(cell, ARRAYSIZE(cell), L"%d,%d", x, y);
    swprintf(edge, ARRAYSIZE(edge), L"%d,%d", x + (side == 3), y + (side == 2));
    const wchar_t *direction = side % 2 ? L"v" : L"h";
    if (tool == EDIT_WALL)
        swprintf(query, ARRAYSIZE(query), L"/level/maze/edge[@dir='%ls' and @edgeIndex='%ls']", direction, edge);
    else if (tool == EDIT_FLOOR)
        swprintf(query, ARRAYSIZE(query), L"/level/maze/floor[@cell='%ls']", cell);
    else if (tool == EDIT_ERASE)
        swprintf(query, ARRAYSIZE(query), L"/level/objectList/*[@cellLoc='%ls']", cell);
    else if (tool == EDIT_POWER)
        swprintf(query, ARRAYSIZE(query), L"/level/objectList/*[@cellLoc='%ls' and (self::freezePickup or self::zoogZapper or self::autoSort or self::fuzzlePop)]", cell);
    else if (tool == EDIT_HERO)
        wcscpy(query, L"/level/objectList/playerSpawner");
    else
        swprintf(query, ARRAYSIZE(query), L"/level/objectList/%ls[@cellLoc='%ls']", objects[tool-EDIT_HERO], cell);
    IXMLDOMNode *old = find_node(query);
    if (!old && tool == EDIT_ERASE) {
        swprintf(query, ARRAYSIZE(query), L"/level/maze/edge[@dir='%ls' and @edgeIndex='%ls']", direction, edge);
        old = find_node(query);
        if (!old) {
            swprintf(query, ARRAYSIZE(query), L"/level/maze/floor[@cell='%ls']", cell);
            old = find_node(query);
        }
    }
    BOOL changed = FALSE, had_old = old != NULL;
    wchar_t patrol[180] = {0};
    if (old && tool==EDIT_ZOOG) {
        BSTR previous_type = read_attribute(old,L"@type");
        if (previous_type && !_wcsicmp(previous_type,monster_types[variant])) {
            BSTR previous_route = read_attribute(old,L"@spawnParams");
            if (previous_route && wcslen(previous_route)<ARRAYSIZE(patrol)) wcscpy(patrol,previous_route);
            SysFreeString(previous_route);
        }
        SysFreeString(previous_type);
    }
    if (old) {
        changed = remove_node(old);
        old->lpVtbl->Release(old);
    }
    if (tool != EDIT_ERASE && (!had_old || tool >= EDIT_HERO)) {
        IXMLDOMNode *parent = find_node(tool < EDIT_HERO ? L"/level/maze" : L"/level/objectList");
        if (!parent && tool >= EDIT_HERO) {
            IXMLDOMNode *root = find_node(L"/level");
            IXMLDOMElement *list = new_element(L"objectList");
            if (append(root, list)) parent = find_node(L"/level/objectList");
            if (list) list->lpVtbl->Release(list);
            if (root) root->lpVtbl->Release(root);
        }
        IXMLDOMElement *item = new_element(tool == EDIT_WALL ? L"edge" : tool == EDIT_FLOOR ? L"floor" : tool == EDIT_POWER ? power_tags[variant] : objects[tool-EDIT_HERO]);
        if (item) {
            if (tool == EDIT_WALL) {
                attribute(item, L"dir", direction);
                attribute(item, L"edgeIndex", edge);
                attribute(item, L"edgeType", L"0");
            } else if (tool == EDIT_FLOOR) {
                attribute(item, L"cell", cell);
                attribute(item, L"tile", L"1");
            } else {
                attribute(item, L"cellLoc", cell);
                if (tool == EDIT_CAGE || tool == EDIT_HOME || (tool == EDIT_POWER && variant == POWER_POP)) attribute(item, L"color", colors[color==EDIT_RAINBOW ? 0 : color]);
                if (tool == EDIT_CAGE) {
                    attribute(item, L"type", L"0");
                    if (color==EDIT_RAINBOW) attribute(item,L"hdRestored",L"Rainbow");
                }
                if (tool==EDIT_POWER && variant==POWER_SLOW) attribute(item,L"hdRestored",L"Slow");
                if (tool == EDIT_HOME) attribute(item, L"minCount", L"0");
                if (tool == EDIT_ZOOG) {
                    attribute(item, L"type", monster_types[variant]);
                    if (variant==ZOOG_DRAGON) attribute(item,L"hdRestored",L"Dragon");
                    const wchar_t *route = L"";
                    if (variant == ZOOG_GUARD || variant == ZOOG_GUARD_FAST) {
                        route = L"P1";
                        if (x+1<board.columns && !(board.tiles[y][x+1]&EDIT_HAS_WALL_V)) route=L"E1, W1";
                        else if (x>0 && !(board.tiles[y][x]&EDIT_HAS_WALL_V)) route=L"W1, E1";
                        else if (y+1<board.rows && !(board.tiles[y+1][x]&EDIT_HAS_WALL_H)) route=L"S1, N1";
                        else if (y>0 && !(board.tiles[y][x]&EDIT_HAS_WALL_H)) route=L"N1, S1";
                    }
                    attribute(item, L"spawnParams", patrol[0] ? patrol : route);
                }
            }
            changed = append(parent, item) || changed;
            item->lpVtbl->Release(item);
        }
        if (parent) parent->lpVtbl->Release(parent);
    }
    if (changed) {
        dirty = TRUE;
        read_board();
    }
    return changed;
}
