import yaml

def read_input(file):
    with open(file, 'r') as f:
        return f.read().splitlines()
    
def get_results(input, destination):

    answers = [0] * 10
    # 1, What is the number of all update messages?
    # 2, What is the number of announcements?
    # 3, What is the number of withdrawals?
    for line in input:
        answers[0] += 1
        l = line.split('|')
        if l[2] == 'A':
            answers[1] += 1
        elif l[2] == 'W':
            answers[2] += 1

    prefix = "number"
    my_dict = {f"{prefix}{i+1}": answers[i] for i in range(len(answers))}
    with open(destination, "w") as f:
        yaml.dump(answers, f, default_flow_style=False)





if __name__ == '__main__':
    get_results(read_input("./routeviews_project_testcases/testcase.txt"), "./results/testcase.yaml")
    get_results(read_input("./input/updates.20150611.0845-0945.txt"), "./results/20150611.0845-0945.yaml")
    get_results(read_input("./input/updates.20150612.0845-0945.txt"), "./results/20150612.0845-0945.yaml")
    get_results(read_input("./input/updates.20150613.0845-0945.txt"), "./results/20150613.0845-0945.yaml")
    


        

        

