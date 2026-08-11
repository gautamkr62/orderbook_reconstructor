/*
Author:  Gautam Kumar
*/
#include <bits/stdc++.h>
using namespace std;

int minCapability(vector<int> &num, int k){
    vector<int> nums = num;
    int n = nums.size();

    if (nums.size() == 2)
    {
        return min(nums[0], nums[1]);
    }
    else if (nums.size() == 3)
    {
        if (k == 1)
        {
            return *min_element(nums.begin(), nums.end());
        }
        else
        {
            return min(nums[0], nums[2]);
        }
    }
    else if (nums.size() == 4)
    {
        if (k == 1)
        {
            return *min_element(nums.begin(), nums.end());
        }
        else
        {
            return min(min(nums[0], nums[2]), min(nums[1], nums[3]));
        }
    }

    vector<int> dp(n + 5, INT_MAX);
    vector<int> numrob(n + 5, 0);
    // dp[i] means ith element is chosen

    dp[0] = nums[0];
    numrob[0] = 1;
    dp[1] = nums[1];
    numrob[1] = 1;
    dp[2] = min(nums[0], nums[2]);
    numrob[2] = 2;
    dp[3] = min(nums[1], nums[3]);
    numrob[3] = 2;
    // if(nums.size()%2 == 0) {
    //     nums.push_back(INT_MAX);
    //     n++;
    // }

    for (int i = 4; i < n; i++)
    {
        if (nums[i] < min(dp[i - 2], dp[i - 3]))
        {
            dp[i] = nums[i];
            numrob[i] = 1 + max(numrob[i - 2], numrob[i - 3]);
        }
        else
        {
            // choose the one which gives you smaller element.
            // here we are making a choice
            // what if taking the smallest element will not reach to correct k
            if (dp[i - 2] < dp[i - 3])
            {
                dp[i] = dp[i - 2];
                numrob[i] = 1 + numrob[i - 2];
            }
            else
            {
                dp[i] = dp[i - 3];
                numrob[i] = 1 + numrob[i - 3];
            }
        }
    }

    if (n % 2 != 0)
    {
        if (numrob[n - 1] != k)
        {
            if (numrob[n - 3] == k - 1)
            {
                return min(dp[n - 3], nums[n - 1]);
            }
            else if (numrob[n - 4] == k - 1)
            {
                return min(dp[n - 4], nums[n - 1]);
            }
        }
    }

    return dp[n - 1];
    /*
        Is there a case that at the end we will not reach to k
        for every 2 element we are adding 1


    */

    // capability = maximum(money from one from all the houses he robbed)

    // k houses he will steal from
    // what is the minimum house he can rob, i guess

    // for each index two things I need to store
    // Including this house how many houses have been robbed
    // what's the minimum so far.
    // so when I calculate
    // numrob[i] = numrob[i-2] or numrob[i-3]
    // dp[i] = min(nums[i], dp[i-2], dp[i-3])

    // what should be stored the number of houses or the min value
    // try to minimize the value as we will get k elements any how, right?

    /*
    If current element is smaller dp[i] < min(dp[i-2], dp[i-3])
    then choose the subsequence which has more elements

    if current element is larger choose the subsequenec having small element.

    // How can one prove that this is correct.
    // isn't it the case that we always have a subsequence
    // k = n/2, we will always have an answer right?
    // can we miss an answer?

    nums =     6 5 4 3 2 1
    dp =       6 5 4 3 2 1
    numrob =   1 1 2 2 3 3


    nums =   2 3 5 9 1
    dp =     2 3 2 3 1
    numrob = 1 1 2 2
    k = 3

    when size is even
    nums =   10 2 6 8 4
    dp   =   10 2 6 2 2
    numrob = 1  1 2 2 2
    k = 3
    at any point numrob == k
    // iterate through the left array find if you get a smaller element than that return it.
    k = 3

    if(nums[i] < min(dp[i-2], dp[i-3])){
        dp[i] = nums[i];
        numrob[i] = 1 + max(numrob[i-2], numrob[i-3]);
    }
    else{
        // choose the one which gives you smaller element.
        if(dp[i-2] < dp[i-3]){
            dp[i] = dp[i-2];
            numrob[i] = 1 + numrob[i-2];
        }
        else {
            dp[i] = dp[i-3];
                numrob[i] = 1 + numrob[i-3];
        }

    }

*/
}

void solve(){
    int n, k;
    cin>>n>>k;
    vector<int> arr(n);
    for(int i = 0;i<n;i++) cin>>arr[i]; 
    std::cout<<"answer\n";
    std::cout<<minCapability(arr, k);
    std::cout<<"end\n"; 
}
int main()
{
    ios_base::sync_with_stdio(false);
    cin.tie(NULL);
    int t;
    cin >> t;
    while (t--)
        solve();
    return 0;
}
/*


*/