from altair as alt
from altair_saver import save

nanosec_to_sec = 1e9
interval_sec = 1.0
exp_num = 6
num_clients = [4, 32, 64, 128, 192, 256]
log_file_dir = "../../build/tests/logging/exp{}/"

def get_tp_lat(exp_id):
    total_throughput = 0
    total_latency = 0
    for id in range(num_clients[exp_id]):
        log_filepath = log_file_dir.format(exp_id)+"client{}log.txt".format(id)
        tokens, latencies = read_datalist(log_filepath)
        total_throughput += calc_single_client_throughput(tokens)
        total_latency += calc_single_client_latency(latencies)
    return total_throughput, total_latency/num_clients[exp_id]

def read_datalist(filename):
    ret_1 = []
    ret_0 = []
    with open(filename) as f:
        for line in f:
            ret_0.append(int(line))
            ret_1.append(int(line))
    return ret_0, ret_1 

def calc_single_client_throughput(time_tokens):
    st_time = time_tokens[0]
    total_secs = 0
    for t in time_tokens:
        if st_time + nanosec_to_sec > t:
            st_time = t
            total_secs = total_secs + 1
    return num_ops/total_secs

def calc_single_client_latency(intervals):
    num_ops = len(intervals)
    total_sec = sum(intervals) / nanosec_to_sec
    avg = total_sec / num_ops
    return avg

def get_all_exp_throughput_latency():
    data_throughput = []
    data_latency = []
    for exp_id in range(exp_num):
        throuput, latency = get_tp_lat(exp_id)
        data_throughput.append(throuput)
        data_latency.append(latency)
    return data_throughput, data_latency

def graph_tp(data):
    n_subject = 1
    save_dir = "tmp/sync_hotstuff_throughput.png"
    data = pd.DataFrame(
        {'Number of Clients':  num_clients,
         'Throughput (Opersations/Second)': data})
    chart = alt.Chart(data).mark_line().encode(x='Number of Clients:Q', y='Throughput (Opersations/Second):Q')
    save(chart, save_dir)

def graph_lat(data):
    n_subject = 1
    save_dir = "tmp/sync_hotstuff_latency.png"
    data = pd.DataFrame(
        {'Number of Clients':  num_clients,
         'Latency': data})
    chart = alt.Chart(data).mark_line().encode(x='Number of Clients:Q', y='Latency:Q')
    save(chart, save_dir)

if __name__ == "__main__":
    assert(exp_num == len(num_clients))
    data_tp, data_lat = get_all_exp_throughput_latency()
    graph_tp(data_tp)
    graph_lat(data_lat)

