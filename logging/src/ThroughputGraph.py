from altair as alt
from altair_saver import save

nanosec_to_sec = 1e9
interval_sec = 1.0
exp_num = 6
num_clients = [4, 32, 64, 128, 192, 256]
log_file_dir = "../../build/tests/logging/exp{}/"

def get_throughput(exp_id):
    total = 0
    for id in range(num_clients[exp_id]):
        log_filepath = log_file_dir.format(exp_id)+"client{}log.txt".format(id)
        tokens = read_datalist(log_filepath)
        total + = calc_single_client_throughput(tokens)
    return total

def read_datalist(filename):
    ret = []
    with open(filename) as f:
        ret.append(int(line) for line in f)
    return ret

def calc_single_client_throughput(time_tokens):
    num_ops = len(time_tokens)
    total_sec = (time_tokens[-1]-time_tokens[0]) / nanosec_to_sec
    avg = num_ops / total_sec
    return avg

def get_all_exp_throughput():
    data = []
    for exp_id in range(exp_num):
        data.append(get_throughput(exp_id))
    return data

def graph(data):
    n_subject = 1
    save_dir = "tmp/sync_hotstuff_throughput.png"
    data = pd.DataFrame(
        {'Number of Clients':  num_clients,
         'Throughput (Opersations/Second)': data})
    chart = alt.Chart(data).mark_line().encode(x='Number of Clients:Q', y='Throughput (Opersations/Second):Q')
    save(chart, save_dir)

if __name__ == "__main__":
    assert(exp_num == len(num_clients))
    graph(get_all_exp_throughput())

