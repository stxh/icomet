#ifndef ICOMET_IPFILTER_H
#define ICOMET_IPFILTER_H

#include "../config.h"
#include <string>
#include <set>

class IpFilter{
private:
	bool deny_all;
	bool allow_all;
	std::set<std::string> deny;
	std::set<std::string> allow;
	
	bool check_hit(const std::set<std::string> &m, const char *ip){
		std::set<std::string>::const_iterator it;
		if(m.empty()){
			return false;
		}
		it = m.upper_bound(ip);
		if(it != m.begin()){
			it --;
		}
		if(strstr(ip, (*it).c_str()) == ip){
			return true;
		}
		return false;
	}
public:
	IpFilter(){
		deny_all = false;
		allow_all = false;
	}
	
	void add_allow(const char *ip_prefix){
		if(strcmp(ip_prefix, "all") == 0 || strcmp(ip_prefix, "*") == 0){
			allow_all = true;
		}else{
			allow.insert(ip_prefix);
		}
	}
	
	void add_deny(const char *ip_prefix){
		if(strcmp(ip_prefix, "all") == 0 || strcmp(ip_prefix, "*") == 0){
			deny_all = true;
		}else{
			deny.insert(ip_prefix);
		}
	}
	
	bool check_pass(const char *ip){
		// check specified allow/deny
		if(check_hit(allow, ip)){
			return true;
		}
		if(check_hit(deny, ip)){
			return false;
		}
		
		if(deny_all){
			return false;
		}
		if(allow_all){
			return true;
		}
		return false;
	}
};

#endif
