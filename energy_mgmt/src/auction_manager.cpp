#include "auction_manager.h"

AuctionManager::AuctionManager(unsigned int robot_id) {
    ROS_INFO("Creating instance of AuctionManager");
    initializeVariables(robot_id);
    loadParameters();
    createSubscribers();
    ROS_INFO("Instance created successfully");
    data_logger = new DataLogger("energy_mgmt", robot_name, log_path);
    data_logger->createLogFile("auctions.log", "#auction_id,sim_time,docking_station_id,robot_id_1,bid_1,...,winner,robot_winner\n");
}

void AuctionManager::initializeVariables(unsigned int robot_id) {
    winner_of_auction = false;
    robot_cannot_participate_to_auctions = false;
    optimal_ds_is_set = false;
    this->robot_id = robot_id;
    auction_participation_state = IDLE;    
    time_last_participation = 0;
    local_auction_id = 0;
    current_auction.starting_time = -1;
    current_auction.ending_time = 0;
}

void AuctionManager::loadParameters() {
    ros::NodeHandle nh_tilde("~");
    int tmp;
    nh_tilde.param<int>("num_robots", tmp, -1);
    if(tmp < 0)
        ROS_FATAL("Invalid number of robots!");
    else
        num_robots = (unsigned int)tmp;
    if(!nh_tilde.getParam("auction_duration", auction_timeout)) {//TODO int or double?
        ROS_ERROR("invalid auction_timeout!!");
        auction_timeout = 3;
    }
    if(!nh_tilde.getParam("extra_auction_time", extra_auction_time)) { //TODO use getParam and the fact that it returns a bool!!! http://wiki.ros.org/roscpp_tutorials/Tutorials/Parameters#getParam.28.29
        extra_auction_time = 3;
        ROS_ERROR("Invalid extra_auction_time!");
    }
    if(!nh_tilde.getParam("reauctioning_timeout", reauctioning_timeout)) {
        ROS_ERROR("invalid reauctioning_timeout");
        reauctioning_timeout = 5;
    }
    if(!nh_tilde.getParam("log_path", log_path))
        ROS_ERROR("invalid log_path"); //TODO during tests this is printed, but why?
    if(!nh_tilde.getParam("robot_prefix", robot_prefix))
        ROS_ERROR("invalid robot_prefix");
    if(!nh_tilde.getParam("robot_prefix", robot_prefix))
        ROS_ERROR("invalid robot_prefix");
    robot_name = robot_prefix.substr(1);
    nh_tilde.param<double>("sleep_time_between_two_participations", sleep_time_between_two_participations, 5); //s //TODO use
}

void AuctionManager::createSubscribers() {
    //TODO do i need 'adhoc_communication' in the service name? and 'send_em_auction'?
    //TODO everytime we use a topic in two different classes, we should load it from a file, so to be sure not to have typos
    auction_starting_topic = "adhoc_communication/send_em_auction/auction_starting"; 
    auction_reply_topic = "adhoc_communication/send_em_auction/auction_reply";
    auction_result_topic = "adhoc_communication/send_em_auction/auction_result"; 

    std::string my_prefix = ""; //TODO
    auction_starting_sub = nh.subscribe(auction_starting_topic, 1000, &AuctionManager::auctionStartingCallback, this);
    auction_reply_sub = nh.subscribe(auction_reply_topic, 1000, &AuctionManager::auctionReplyCallback, this);
    auction_result_sub = nh.subscribe(auction_result_topic, 1000, &AuctionManager::auctionResultCallback, this);
}

void AuctionManager::logMetadata()
{
    ROS_INFO("Creating log files...");

    /* Create directory */
    log_path = log_path.append("/energy_mgmt");
    log_path = log_path.append(robot_prefix);
    boost::filesystem::path boost_log_path(log_path.c_str());
    if (!boost::filesystem::exists(boost_log_path))
    {
        ROS_INFO("Creating directory %s", log_path.c_str());
        try
        {
            if (!boost::filesystem::create_directories(boost_log_path))
            {
                ROS_ERROR("Cannot create directory %s: aborting node...", log_path.c_str());
                exit(-1);
            }
        }
        catch (const boost::filesystem::filesystem_error &e)
        {
            ROS_ERROR("Cannot create path %saborting node...", log_path.c_str());
            exit(-1);
        }
    }
    else
    {
        ROS_INFO("Directory %s already exists: log files will be saved there", log_path.c_str());
    }

    std::string filename;
    std::fstream fs;

    log_path = log_path.append("/");
    filename = log_path + std::string("auctions.csv");

    fs.open(filename.c_str(), std::fstream::in | std::fstream::app | std::fstream::out);
    fs << "#auction_duration,reauctioning_timeout,extra_auction_time" << std::endl;
    fs << auction_timeout << "," << reauctioning_timeout << "," << extra_auction_time << std::endl;
    fs.close();
}

void AuctionManager::tryToAcquireDs() {
    ROS_INFO("Robot is going to try to start a new auction");

    if(auction_participation_state == PARTICIPATING)
        ROS_INFO("The robot is already participating to an auction: let's wait for the other auction to terminate");
    else {
        auction_participation_state = MANAGING;
        current_auction = startNewAuction();

        ROS_INFO("Starting new auction (%d)", current_auction.auction_id);

        auction_bids.clear();

        bid_t bid;
        bid.robot_id = robot_id;
        bid.bid = bid_computer->getBid();
        auction_bids.push_back(bid);
        sender->sendNewAuction(bid, current_auction, auction_starting_topic, robot_id);

        scheduleAuctionTermination();
    }
}

auction_t AuctionManager::startNewAuction() {    
    auction_t new_auction;
    new_auction.auction_id = nextAuctionId();
    new_auction.auctioneer = robot_id;
    new_auction.starting_time = time_manager->simulationTimeNow().toSec();
    new_auction.ending_time = -1;
    new_auction.docking_station_id = optimal_ds_id;
    if(!optimal_ds_is_set) {
        ROS_FATAL("OPTIMAL DS IS NOT SET!!!");
        exit(-1);
    }
    return new_auction;
}

void AuctionManager::scheduleAuctionTermination() { //FIXME put also some safety procedure to avoid "un-termination"
    terminate_auction_timer = nh.createTimer(ros::Duration(auction_timeout), &AuctionManager::terminateAuctionCallback, this, true, true); //arguments: Duration _period, const TimerCallback &_callback, CallbackQueueInterface *_queue, bool oneshot=false, bool autostart=true (http://docs.ros.org/indigo/api/roscpp/html/structros_1_1TimerOptions.html)
}

void AuctionManager::terminateAuctionCallback(const ros::TimerEvent &event) {
    auction_mutex.lock();

    if(auction_participation_state != MANAGING)
        ROS_ERROR("Auction %d has been aborted because a more recent auction (%d) has been started by another robot, but this should not happen given the current implementation...", 1, current_auction.auction_id); //TODO auction_id
    else {
        ROS_INFO("Auction %d has terminated: computing winner", current_auction.auction_id);
        unsigned int winner_id = computeAuctionWinner();
        winner_of_auction = isThisRobotTheWinner(winner_id);
        current_auction.ending_time = time_manager->simulationTimeNow().toSec();
        current_auction.winner_robot = winner_id;

        bid_t bid;
        bid.robot_id = winner_id;
        sendAuctionResult(bid);
    }

    auction_participation_state = IDLE;

    auction_mutex.unlock();
}

unsigned int AuctionManager::computeAuctionWinner() {
    bid_t winning_bid;
    winning_bid.robot_id = -1;
    winning_bid.bid = std::numeric_limits<float>::min();

    std::stringstream stream;
    stream << current_auction.auction_id << "," << ros::Time::now() << "," << current_auction.docking_station_id << ",";
    for (auto it = auction_bids.begin(); it != auction_bids.end(); it++)
    {
        ROS_DEBUG("robot_%d placed %.1f", it->robot_id, it->bid);
        stream << it->robot_id << "," << it->bid << ",";
        if (it->bid > winning_bid.bid)
        {
            winning_bid.robot_id = it->robot_id;
            winning_bid.bid = it->bid;
        }
    }
    stream << "winner," << winning_bid.robot_id << std::endl;
    data_logger->updateLogFile("auctions.log", stream);

    if(winning_bid.robot_id < 0) // this should not happen, since at least the robot that started the auction must have placed a bid
        ROS_ERROR("No winner for auction %d has been found", current_auction.auction_id); //TODO raise exception?

    ROS_DEBUG("The winner of auction %d is robot_%d", current_auction.auction_id, winning_bid.robot_id);
    return winning_bid.robot_id;
}

bool AuctionManager::isThisRobotTheWinner(unsigned int winner_id) {
    if (winner_id == robot_id)
    {
        ROS_INFO("Winner of its own auction");  // TODO(minor) specify which auction
        ROS_INFO("winner_of_auction = true");
//        set_l5(true);
        return true;
    }
    else
    {
        ROS_INFO("Robot lost its own auction");
        ROS_INFO("winner_of_auction = false");
        return false;
    }
}

void AuctionManager::sendAuctionResult(bid_t bid) {
    ROS_INFO("Send results for auction %d to other robots", current_auction.auction_id);
    std::vector<unsigned int> participants;
    for(auto it = auction_bids.begin(); it != auction_bids.end(); it++)
        participants.push_back(it->robot_id);
    sender->sendResults(bid, current_auction, auction_result_topic, robot_id, participants);
}

void AuctionManager::auctionReplyCallback(const adhoc_communication::EmAuction::ConstPtr &msg)
{
    auction_mutex.lock();
    ROS_DEBUG("Received bid (%.2f) from robot %d for auction %u", msg.get()->bid.bid, msg.get()->bid.robot, (unsigned int)msg.get()->auction);

//    if(auction_participation_state != MANAGING)
//        ROS_INFO("The robot received a bid, but it is not managing an auction: ignore it");
    
    if(robot_id != msg.get()->robot)
        ROS_ERROR("The received bid is for an auction started by robot_%u, which is not this robot", msg.get()->robot);
    
    else if (current_auction.auction_id != msg.get()->auction) 
        ROS_ERROR("The received bid is not for the auction recently started by this robot (current auction is %d, received is %d): ignore it", current_auction.auction_id, msg.get()->auction);
            
    else if (current_auction.docking_station_id != (unsigned int)msg.get()->docking_station) 
        ROS_ERROR("The received bid is for ds_%u, which is not the one under auction (it's ds_%u)", (unsigned int)msg.get()->docking_station, current_auction.docking_station_id);
            
    else {
        ROS_DEBUG("Bid accepted");
        bid_t bid;
        bid.robot_id = msg.get()->bid.robot;
        bid.bid = msg.get()->bid.bid;
        auction_bids.push_back(bid);
    }

    auction_mutex.unlock();
}

bool AuctionManager::isRobotParticipatingToAuction() {
    if((auction_participation_state == PARTICIPATING || auction_participation_state == MANAGING) && current_auction.ending_time > 0)
        ROS_ERROR("impossible..."); //TODO better msg
    return ((auction_participation_state == PARTICIPATING || auction_participation_state == MANAGING) && current_auction.ending_time < 0);
}

bool AuctionManager::isRobotWinnerOfMostRecentAuction() {
    if(isRobotParticipatingToAuction())
        ROS_ERROR("Ideally, isRobotWinnerOfMostRecentAuction() shoud not be called when the robot is participating to an auction...");
    return winner_of_auction; //TODO check also that current_auction.winner_robot is this robot
}

unsigned int AuctionManager::nextAuctionId()
{
    ROS_DEBUG("Compute next auction ID");
    // Examples:
    //   With 15 robots:
    //     23 * pow(10, (ceil(log10(15)))) + 11 = 23 * 10^2 + 11 = 2300 + 11 = 2311;
    //     23 * pow(10, (ceil(log10(15)))) +  9 = 23 * 10^2 +  9 = 2300 +  9 = 2309;
    //   With 10 robots:
    //     50 * pow(10, (ceil(log10(10)))) +  8 = 50 * 10^1 +  8 =  500 +  8 =  508;
    //     50 * pow(10, (ceil(log10(10)))) +  7 = 50 * 10^1 +  8 =  500 +  9 =  508;
    
    local_auction_id++;
    unsigned int id = local_auction_id * pow(10, (ceil(log10(num_robots)))) + robot_id;
    ROS_DEBUG("%u", id);
    return id;
}

void AuctionManager::auctionResultCallback(const adhoc_communication::EmAuction::ConstPtr &msg) { //TODO check if completed tested (code coverage, calls of public functions, ...)
    auction_mutex.lock();

    if(current_auction.auction_id == msg.get()->auction) {
    
        bool found = false;
        for(unsigned int i=0; i < msg.get()->participants.size() && !found; i++)
            if(msg.get()->participants[i] == robot_id)
                found = true;
        if(!found)
            ROS_ERROR("robot sent bid for auction %u by robot %u but was not received!!!", current_auction.auction_id, current_auction.auctioneer);
    
        if ((unsigned int)msg.get()->winning_robot == robot_id) //TODO all ids should be unsigned int
        {
            ROS_INFO("Winner of the auction started by another robot");

            if(current_auction.docking_station_id == optimal_ds_id && msg.get()->docking_station == optimal_ds_id) {
                winner_of_auction = true;
                ROS_INFO("winner_of_auction = true");
//                set_l5(true);
            }
            else {
                ROS_INFO("The robot won an auction, but meanwhile it changed it's optimal DS and it is no more the auctioned one: ignoring auction result");
                winner_of_auction = false;
                ROS_INFO("winner_of_auction = false");
            }
        }
        else
        {
            ROS_INFO("Robot didn't win this auction started by another robot");
            winner_of_auction = false;
            ROS_INFO("winner_of_auction = false");
        }
        terminate_auction_timer.stop();

        current_auction.ending_time = time_manager->simulationTimeNow().toSec();
        current_auction.winner_robot = (unsigned int)msg.get()->winning_robot;
        auction_participation_state = IDLE;

    } else
        ROS_INFO("Not the current auction, ignoring");

    auction_mutex.unlock();
}

void AuctionManager::auctionStartingCallback(const adhoc_communication::EmAuction::ConstPtr &msg) //TODO check if completed tested (code coverage, calls of public functions, ...)
{
    auction_mutex.lock();

    ROS_INFO("Received new auction (%d) from robot_%u for ds_%u", msg.get()->auction, msg.get()->robot, msg.get()->docking_station);

    if(optimal_ds_is_set && (unsigned int)msg.get()->docking_station == optimal_ds_id) {
        if(auction_participation_state == MANAGING) {
            ROS_INFO("This robot is currently managing an auction...");
            if(current_auction.starting_time < msg.get()->starting_time) {
                ROS_INFO("... and the auction managed by the robot is older than the auction started by the other robot: the auction of this robot will be discarded");
                current_auction = participateToOtherRobotAuction(bid_computer->getBid(), msg); //TODO I have to send a bid even if it is sure to be unwinning because I have to force participation... code should be improved...
            } else
                ROS_INFO("... and the auction managed by the robot is more recent than the auction started by the other robot: ignoring the auction started by the other robot");
        }
        
        else if(robot_cannot_participate_to_auctions) //TODO this is not tested properly, probably
            ROS_INFO("robot is searching for a path on graph: ignore auction");  
            
        else {
            double bid_double = bid_computer->getBid();
            if (bid_double > msg.get()->bid.bid)
            {
                ROS_INFO("The robot can place an higher bid than the one received");
                current_auction = participateToOtherRobotAuction(bid_double, msg);
            }
            else
                ROS_INFO("The robot has no chance to win, so it won't place a bid for this auction");
        }

    }
    else {
        if(!optimal_ds_is_set)
            ROS_INFO("Robot has not selected a DS yet, so it cannot take part to the auction");
        else
            ROS_INFO("Robot is not interested in ds%d (it wants ds%d)", (unsigned int)msg.get()->docking_station, optimal_ds_id);
    }

    auction_mutex.unlock();
}

//TODO check if completed tested (code coverage, calls of public functions, ...)
auction_t AuctionManager::participateToOtherRobotAuction(double bid_double, const adhoc_communication::EmAuction::ConstPtr &msg) { //TODO bad arg name
    ROS_INFO("Participating to auction started by another robot");
    auction_participation_state = PARTICIPATING;
    terminate_auction_timer = nh.createTimer(ros::Duration(auction_timeout + extra_auction_time), 
                                             &AuctionManager::endAuctionParticipationCallback, this, true, true);

    auction_t new_auction;
    new_auction.auctioneer = msg.get()->robot;
    new_auction.auction_id = msg.get()->auction;
    new_auction.docking_station_id = msg.get()->docking_station;
    new_auction.starting_time = msg.get()->starting_time;
    new_auction.ending_time = -1;

    bid_t bid;
    bid.robot_id = robot_id;
    bid.bid = bid_double;
    sender->sendBid(bid, new_auction, auction_reply_topic, robot_id);

    return new_auction;
}

void AuctionManager::endAuctionParticipationCallback(const ros::TimerEvent &event) 
{
    // FIXME it seems not to work correctly sometimes... keep checked (there is a sanity check in AuctionObserver)
    auction_mutex.lock();
    if(auction_participation_state == PARTICIPATING) {
        ROS_DEBUG("Force to consider auction concluded");
        auction_participation_state = IDLE;
        current_auction.ending_time = time_manager->simulationTimeNow().toSec();
    //    current_auction.winner_robot = ??? //TODO
        winner_of_auction = false;
        ROS_INFO("winner_of_auction = false");
    }
    auction_mutex.unlock();
}

void AuctionManager::lock() {
    auction_mutex.lock();
}

void AuctionManager::unlock() {
    auction_mutex.unlock();
}

void AuctionManager::setOptimalDs(unsigned int optimal_ds_id) {
    this->optimal_ds_id = optimal_ds_id;
    optimal_ds_is_set = true;
}

void AuctionManager::preventParticipationToAuctions() {
    robot_cannot_participate_to_auctions = true;
}

void AuctionManager::allowParticipationToAuctions() {
    robot_cannot_participate_to_auctions = false;
}

auction_t AuctionManager::getCurrentAuction() {
    return current_auction;
}

void AuctionManager::setBidComputer(BidComputer *bid_computer) {
    this->bid_computer = bid_computer;
}

void AuctionManager::setTimeManager(TimeManagerInterface *time_manager) {
    this->time_manager = time_manager;
}

void AuctionManager::setSender(Sender *sender) {
    this->sender = sender;
}
