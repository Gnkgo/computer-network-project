import yaml
from tqdm import tqdm
from collections import defaultdict


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
    prefix_set = set()
    prefix_announce_set = set()
    prefix_withdraw_set = set()
    answers = [0] * 14

    # Dictionary to keep track of the timestamps of the most recent update for each prefix
    last_update_time = {}
    longest_update_time = {}

    # Dictionary to keep track of the number of bursts for each prefix
    burst_count = defaultdict(int)
    

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
        l = line.split('|')
        answers[0] += 1
        prefix = l[5]
        timestamp = l[1]
        

        if l[2] == 'A':
            answers[1] += 1
            prefix_announce_set.add(prefix)
        elif l[2] == 'W':
            answers[2] += 1
            prefix_withdraw_set.add(prefix)
                    
        if (prefix in longest_update_time):
            if (int(timestamp) - longest_update_time[prefix] > 240):
                burst_count[prefix] += 1

        longest_update_time[prefix] = int(timestamp)
        last_update_time[prefix] = int(timestamp)
        prefix_set.add(prefix)
    
    answers[3] = len(prefix_set)
    answers[4] = len(prefix_announce_set)
    answers[5] = len(prefix_withdraw_set)
    answers[6] = sum(burst_count.values())
    answers[7] = max(burst_count.values())
    answers[8] = max(last_update_time.values()) - min(last_update_time.values())

        

    prefix = "number"
    data = {i+1: num for i, num in enumerate(answers)}
    with open(destination, "w") as f:
        yaml.dump(data, f)





if __name__ == '__main__':
    get_results("./test.txt", "./results/test.yaml")
    get_results("./routeviews_project_testcases/testcase.txt", "./results/testcase.yaml")
    #get_results(read_input("./input/updates.20150611.0845-0945.txt"), "./results/20150611.0845-0945.yaml")
    #get_results(read_input("./input/updates.20150612.0845-0945.txt"), "./results/20150612.0845-0945.yaml")
    #get_results(read_input("./input/updates.20150613.0845-0945.txt"), "./results/20150613.0845-0945.yaml")
    


        

        

