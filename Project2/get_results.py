import yaml
from tqdm import tqdm
from collections import defaultdict
import math

'''
0) BGP Protocol
1) timestamp (in epoch format)
2) W/A/B (withdrawal/announcement/routing table)
3) Peer IP (address of the monitor)
4) Peer ASN (ASN of the monitor)
5) Prefix
6) ASPath (as a list of AS numbers separated by space)
7) Origin Protocol (typically always IGP)
8) Next Hop
9) LocalPref
10) MED
11) Community strings
12) Atomic Aggregator
13) Aggregator

We define an update burst as the sequence of more than one BGP update messages received for
the same prefix, where the time interval between the timestamps of each two consecutive update
messages in the sequence is shorter than 4 minutes. Each burst is an indicator of an event at the
destination network (prefix originator). Given this definition, answer the remaning questions:

'''
def get_results(input_file, destination):
    with open(input_file, 'r') as f:
        updates = f.readlines()

    num_messages = set()
    num_announce = set()
    num_withdraw = set()
    answers = [0] * 14
    longest_burst = defaultdict(int)
    message_interval_count = defaultdict(int)
    start_time = defaultdict(int)
    

    # Dictionary to keep track of the timestamps of the most recent update for each prefix
    last_update_time = defaultdict(int)

    # Dictionary to keep track of the number of bursts for each prefix
    burst_count = defaultdict(int)    
    burst_duration = defaultdict(int)

    num = 0

    # 0, What is the number of all update messages?
    # 1, What is the number of announcements?
    # 2, What is the number of withdrawals?
    # 3. What is the number of prefixes for which at least one BGP update is received?
    # 4. What is the number of prefixes for which at least one announcement is received?
    # 5. What is the number of prefixes for which at least one withdrawals is received?
    # 6. What is the total number of bursts?
    # 7. What is the maximum number of bursts per prefix?
    # 8. How long is the longest burst (in seconds)?
    # 9. What is the average of the longest burst of all prefixes (in seconds)?
    # 10. How many prefixes experience no bursts at all?
    # 11. How many prefixes experience an average burst longer than 10 minutes?
    # 12. How many prefixes experience an average burst longer than 20 minutes?
    # 13. How many prefixes experience an average burst longer than 30 minutes?
    for line in tqdm(updates):
        l = line.strip().split('|')
        prefix = l[5]
        timestamp = int(l[1])
        wab = l[2]

        answers[0] += 1
        num_messages.add(prefix)

        if (prefix in last_update_time and timestamp - last_update_time[prefix] >= 240):
            message_interval_count[prefix] = 0
            start_time[prefix] = timestamp

        message_interval_count[prefix] += 1

        if message_interval_count[prefix] == 2:
            burst_count[prefix] += 1

        if (prefix not in longest_burst):
            longest_burst[prefix] = 0

        if (prefix not in start_time):
            start_time[prefix] = timestamp

        if message_interval_count[prefix] > 1:
            duration = timestamp - start_time[prefix]
            duration_burst = timestamp - last_update_time[prefix]
            longest_burst[prefix] = max(longest_burst[prefix], duration)
            burst_duration[prefix] += duration_burst

        last_update_time[prefix] = timestamp

        if wab == 'A':
            answers[1] += 1
            num_announce.add(prefix)
        elif wab == 'W':
            answers[2] += 1
            num_withdraw.add(prefix)        
    
    answers[3] = len(num_messages)
    answers[4] = len(num_announce)
    answers[5] = len(num_withdraw)
    answers[6] = sum(burst_count.values())
    answers[7] = max(burst_count.values())
    answers[8] = max(longest_burst.values())
    answers[9] = math.ceil((sum(longest_burst.values())) / len(longest_burst))

    for num in num_messages:
        if num not in burst_count:
            answers[10] += 1

    for prefix in burst_count:
        avg_burst_duration = burst_duration[prefix] / burst_count[prefix]
        if (avg_burst_duration > 600):
            answers[11] += 1
        if (avg_burst_duration > 1200):
            answers[12] += 1
        if (avg_burst_duration > 1800):
            answers[13] += 1
        
    prefix = "number"
    data = {i+1: num for i, num in enumerate(answers)}
    with open(destination, "w") as f:
        yaml.dump(data, f)

if __name__ == '__main__':
    get_results("./routeviews_project_testcases/test.txt", "./results/test.yaml")
    get_results("./routeviews_project_testcases/testcase.txt", "./results/testcase.yaml")
    get_results("./input/updates.20150611.0845-0945.txt", "./results/updates.20150611.0845-0945.yaml")
    get_results("./input/updates.20150612.0845-0945.txt", "./results/updates.20150612.0845-0945.yaml")
    get_results("./input/updates.20150613.0845-0945.txt", "./results/updates.20150613.0845-0945.yaml")
    


        

        

