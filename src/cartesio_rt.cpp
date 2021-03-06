#include "cartesio_rt.h"

using namespace XBot;
using namespace XBot::Cartesian;

bool CartesioRt::on_initialize()
{
    setJournalLevel(Journal::Level::Low);

    jinfo("loading..");

    _nh = std::make_unique<ros::NodeHandle>();

    /* Get ik problem from ros param */
    YAML::Node ik_yaml;
    std::string ik_ros_param;
    if(getParam("~problem_description/content", ik_yaml))
    {   
        jinfo("found ik file via config");
    }
    else if(getParam("~problem_param", ik_ros_param))
    {
        std::string ik_str;

        if(!_nh->getParam(ik_ros_param, ik_str))
        {
            jerror("ros param '{}' not found", ik_ros_param);
            return false;
        }

        jinfo("found ik file via ros param");

        ik_yaml = YAML::Load(ik_str);
    }


    /* Create model and ci for rt loop */
    _rt_model = ModelInterface::getModel(_robot->getConfigOptions());

    auto rt_ctx = std::make_shared<Cartesian::Context>(
        std::make_shared<Parameters>(getPeriodSec()),
        _rt_model);

    ProblemDescription ik_problem(ik_yaml, rt_ctx);

    auto impl_name = getParamOr<std::string>("~solver", "OpenSot");

    _rt_ci = CartesianInterfaceImpl::MakeInstance(impl_name, ik_problem, rt_ctx);
    _rt_ci->enableOtg(rt_ctx->params()->getControlPeriod());
    _rt_ci->update(0, 0);

    /* Create model and ci for nrt loop */
    auto nrt_model = ModelInterface::getModel(_robot->getConfigOptions());

    auto nrt_ctx = std::make_shared<Cartesian::Context>(
        std::make_shared<Parameters>(*_rt_ci->getContext()->params()),
        nrt_model);

    _nrt_ci = std::make_shared<LockfreeBufferImpl>(_rt_ci.get(), nrt_ctx);
    _nrt_ci->pushState(_rt_ci.get(), _rt_model.get());
    _nrt_ci->updateState();
    auto nrt_ci = _nrt_ci;

    /*  Create ros api server */
    RosServerClass::Options opt;
    opt.tf_prefix = getParamOr<std::string>("~tf_prefix", "ci");
    opt.ros_namespace = getParamOr<std::string>("~ros_ns", "cartesian");
    auto ros_srv = std::make_shared<RosServerClass>(_nrt_ci, opt);

    /* Initialization */
    _rt_active = false;
    auto rt_active_ptr = &_rt_active;

    _nrt_exit = false;
    auto nrt_exit_ptr = &_nrt_exit;

    _qdot = _q.setZero(_rt_model->getJointNum());
    _robot->getPositionReference(_qmap);

    /* Spawn thread */
    _nrt_th = std::make_unique<thread>(
        [rt_active_ptr, nrt_exit_ptr, nrt_ci, ros_srv]()
        {
            return;

            this_thread::set_name("acartesio_nrt");

            while(!*nrt_exit_ptr)
            {
                this_thread::sleep_for(10ms);

                if(!*rt_active_ptr) continue;

                nrt_ci->updateState();
                ros_srv->run();

            }

        });

    /* Set robot control mode */
    _robot->setControlMode(ControlMode::Position() + ControlMode::Effort());

    /* Feedback */
    _enable_feedback = getParamOr("~enable_feedback", false);

    if(_enable_feedback)
    {
        jinfo("running with feedback enabled");
    }

    return true;
}

void CartesioRt::starting()
{
    // we use a fake time, and integrate it by the expected dt
    _fake_time = 0;

    // align model to current position reference
    _robot->sense(false);
    _robot->getPositionReference(_qmap);
    _rt_model->setJointPosition(_qmap);
    _rt_model->update();

    // reset ci
    _rt_ci->reset(_fake_time);

    // signal nrt thread that rt is active
    // _rt_active = true;

    // transit to run
    start_completed();

}

void CartesioRt::run()
{
    /* Receive commands from nrt */
    // _nrt_ci->callAvailable(_rt_ci.get());

    /* Update robot */
    if(_enable_feedback)
    {
        _robot->sense(false);
        _rt_model->syncFrom(*_robot);

        // TBD floating base state
    }

    /* Solve IK */
    if(!_rt_ci->update(_fake_time, getPeriodSec()))
    {
        jerror("unable to solve \n");
        return;
    }

    /* Integrate solution */
    if(!_enable_feedback)
    {
        _rt_model->getJointPosition(_q);
        _rt_model->getJointVelocity(_qdot);
        _q += getPeriodSec() * _qdot;
        _rt_model->setJointPosition(_q);
        _rt_model->update();
    }


    _fake_time += getPeriodSec();

    /* Send state to nrt */
    // _nrt_ci->pushState(_rt_ci.get(), _rt_model.get());

    /* Move robot */
    if(_enable_feedback)
    {
        _robot->setReferenceFrom(*_rt_model, Sync::Effort);
    }
    else
    {
        _robot->setReferenceFrom(*_rt_model);
    }

    _robot->move();
}

void CartesioRt::stopping()
{
    _rt_active = false;
    stop_completed();
}

void CartesioRt::on_abort()
{
    _rt_active = false;
    _nrt_exit = true;
}

void CartesioRt::on_close()
{
    _nrt_exit = true;
    jinfo("joining with nrt thread..");
    if(_nrt_th) _nrt_th->join();
}

XBOT2_REGISTER_PLUGIN(CartesioRt,
                      cartesio_rt);