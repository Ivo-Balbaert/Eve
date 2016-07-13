#include <runtime.h>
#include <unix/unix.h>
#include <http/http.h>
#include <unistd.h>
#include <luanne.h>

typedef struct json_session {
    heap h;
    table current_session;
    table current_delta;
    table persisted;
    buffer_handler write; // to weboscket
    uuid event_uuid;
    buffer graph;
    table scopes;
    bag root, session;
    boolean tracing;
    evaluation s;
    heap eh;
} *json_session;

extern thunk ignore;

static CONTINUATION_1_0(send_destroy, heap);
static void send_destroy(heap h)
{
    destroy(h);
}

static void format_vector(buffer out, vector v)
{
    int start = 0;
    vector_foreach(v, i){
        int count = 0;
        if (start++ != 0) bprintf(out, ",");
        bprintf(out, "[");
        vector_foreach(i, j){
            print_value_json(out, j);
            if (count ++ < 2) {
                bprintf(out, ",  ");
            }
        }
        bprintf(out, "]");
    }
}

// always call this guy independent of commit so that we get an update,
// even on empty, after the first evaluation. warning, destroys
// his heap
static void send_guy(heap h, buffer_handler output, values_diff diff)
{
    string out = allocate_string(h);
    bprintf(out, "{\"type\":\"result\", \"insert\":[");
    format_vector(out, diff->insert);
    bprintf(out, "], \"remove\": [");
    format_vector(out, diff->remove);
    bprintf(out, "]}");
    apply(output, out, cont(h, send_destroy, h));
}

// for tracing we want to be able to send the structure of the machines
// that we build as a json message
static void send_node_graph(heap h, buffer_handler output, node head, table counts, string parse)
{
    string out = allocate_string(h);
    u64 time = (u64)table_find(counts, sym(time));
    u64 iterations = (u64)table_find(counts, sym(iterations));

    bprintf(out, "{\"type\":\"node_graph\", \"total_time\": %t, \"iterations\": %d, \"head\": \"%v\", \"nodes\":{", time, iterations, head->id);
    vector to_scan = allocate_vector(h, 10);
    vector_insert(to_scan, head);
    int nodeComma = 0;
    vector_foreach(to_scan, n){
        node current = (node) n;
        if(nodeComma) {
            bprintf(out, ",");
        }
        bprintf(out, "\"%v\": {\"id\": \"%v\", \"type\": %v, \"arms\": [", current->id, current->id, current->type);
        int needsComma = 0;
        vector_foreach(current->arms, arm) {
            vector_insert(to_scan, arm);
            if(needsComma) {
                bprintf(out, ",");
            }
            bprintf(out, "\"%v\"", ((node)arm)->id);
            needsComma = 1;
        }
        bprintf(out, "]");

        int* count = table_find(counts, current);
        if(count) {
            bprintf(out, ", \"count\": %u", *count);
        }

        if(current->type == intern_cstring("scan")) {
            bprintf(out, ", \"scan_type\": %v", vector_get(vector_get(current->arguments, 0), 0));
        }
        bprintf(out, "}");
        nodeComma = 1;
    }

    bprintf(out, "}, \"parse\": ");

    buffer_append(out, bref(parse, 0), buffer_length(parse));
    bprintf(out, "}");
    apply(output, out, ignore);
}



// solution should already contain the diffs against persisted...except missing support (diane)
static CONTINUATION_1_2(send_response, json_session, table, table);
static void send_response(json_session js, table solution, table counters)
{
    heap h = allocate_rolling(pages, sstring("response"));
    heap p = allocate_rolling(pages, sstring("response delta"));
    table results = create_value_vector_table(p);
    
    bag_foreach(js->session, e, a, v, c)
        table_set(results, build_vector(p, e, a, v), etrue);

    bag ev = table_find(solution, js->event_uuid);
    if (ev){
        bag_foreach(ev, e, a, v, c) 
            table_set(results, build_vector(p, e, a, v), etrue);
    }

    table_foreach(js->persisted, k, scopeBag) {
        table_foreach(edb_implications(scopeBag), k, impl) {
            if(impl) {
                send_node_graph(h, js->write, impl, counters, js->graph);
            }
        }
    }

    values_diff diff = diff_value_vector_tables(p, js->current_delta, results);
    // destructs h
    send_guy(h, js->write, diff);

    destroy(js->current_delta->h);
    js->current_delta = results;
}

void send_parse(json_session js, buffer query)
{
    string out = allocate_string(js->h);
    interpreter lua = get_lua();
    value json = lua_run_module_func(lua, query, "parser", "parseJSON");
    estring json_estring = json;
    buffer_append(out, json_estring->body, json_estring->length);
    free_lua(lua);
    // send the json message
    apply(js->write, out, ignore);
}


extern heap uuid_heap;
CONTINUATION_1_3(handle_json_query, json_session, bag, uuid, thunk);
void handle_json_query(json_session j, bag in, uuid root, thunk c)
{
    estring t = lookupv(in, root, sym(type));
    estring q = lookupv(in, root, sym(query));
    buffer desc;
    string x = q?alloca_wrap_buffer(q->body, q->length):0;

    if (in == 0) {
        close_evaluation(j->s);
        destroy(j->h);
        return;
    }
    if (t == sym(query)) {
        inject_event(j->s, x, j->tracing);
    }
    if (t == sym(swap)) {
        edb_clear_implications(j->root);
        vector nodes = compile_eve(j->h, x, j->tracing,  &desc);
        vector_foreach(nodes, node) {
            edb_register_implication(j->root, node);
        }
        j->s = build_evaluation(j->scopes, j->persisted, cont(j->h, send_response, j));
        run_solver(j->s);
    }
    if (t == sym(parse)) {
        send_parse(j, alloca_wrap_buffer(q->body, q->length));
    }
}


CONTINUATION_3_3(new_json_session,
                 bag, boolean, buffer, 
                 buffer_handler, table, buffer_handler *)
void new_json_session(bag root, boolean tracing, buffer graph, buffer_handler write, table headers, buffer_handler *handler)
{
    heap h = allocate_rolling(pages, sstring("session"));
    uuid su = generate_uuid();
    json_session j = allocate(h, sizeof(struct json_session));
    j->h = h;
    j->root = root;
    j->tracing = tracing;
    
    j->session = create_bag(h, su);
    j->current_delta = create_value_vector_table(allocate_rolling(pages, sstring("trash")));
    j->event_uuid = generate_uuid();
    j->graph = graph;

    j->persisted = create_value_table(h);
    uuid ru = edb_uuid(root);
    table_set(j->persisted, ru, j->root);
    table_set(j->persisted, su, j->session);
    
    j->scopes = create_value_table(j->h);
    table_set(j->scopes, intern_cstring("session"), su);
    table_set(j->scopes, intern_cstring("all"), ru);
    table_set(j->scopes, intern_cstring("event"), j->event_uuid);
    j->eh = allocate_rolling(pages, sstring("eval"));
    j->s = build_evaluation(j->scopes, j->persisted, cont(j->h, send_response, j));
    
    *handler = websocket_send_upgrade(j->eh, headers, write,
                                      parse_json(j->eh, j->session, cont(h, handle_json_query, j)), 
                                      &j->write);
    buffer desc;
    inject_event(j->s, aprintf(j->h,"init!\n   maintain\n      [#session-connect]\n"), j->tracing);
}

void init_json_service(http_server h, bag root, boolean tracing, buffer graph)
{
    http_register_service(h, cont(init, new_json_session, root, tracing, graph), sstring("/ws"));
}
