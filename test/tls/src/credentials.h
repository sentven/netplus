/*
* (C) 2014,2015 Jack Lloyd
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#ifndef EXAMPLE_CREDENTIALS_MANAGER_H_
#define EXAMPLE_CREDENTIALS_MANAGER_H_

#include <botan_all.h>
#include <memory>

inline bool value_exists(const std::vector<std::string>& vec,
                         const std::string& val)
   {
   for(size_t i = 0; i != vec.size(); ++i)
      {
      if(vec[i] == val)
         {
         return true;
         }
      }
   return false;
   }

class Basic_Credentials_Manager : public Botan::Credentials_Manager
   {
   public:
      Basic_Credentials_Manager(bool use_system_store,
                                const std::string& ca_paths)
         {
         std::vector<std::string> paths;

         if(ca_paths.empty() == false)
            paths.push_back(ca_paths);

         if(use_system_store)
            {
            paths.push_back("/etc/ssl/certs");
            paths.push_back("/usr/share/ca-certificates");
            }

         if(paths.empty() == false)
            {
            load_certstores(paths);
            }
         }

      Basic_Credentials_Manager(Botan::RandomNumberGenerator& rng,
                                const std::string& server_crt,
                                const std::string& server_key)
         {
         Certificate_Info cert;

         cert.key.reset(Botan::PKCS8::load_key(server_key, rng));

         Botan::DataSource_Stream in(server_crt);
         while(!in.end_of_data())
            {
            try
               {
               cert.certs.push_back(Botan::X509_Certificate(in));
               }
            catch(std::exception&)
               {
               }
            }

         // TODO: attempt to validate chain ourselves

         m_creds.push_back(cert);
         }

      void load_certstores(const std::vector<std::string>& paths)
         {
         try
            {
            for(auto const& path : paths)
               {
               std::shared_ptr<Botan::Certificate_Store> cs(new Botan::Certificate_Store_In_Memory(path));
               m_certstores.push_back(cs);
               }
            }
         catch(std::exception&)
            {
            }
         }

      std::vector<Botan::Certificate_Store*>
      trusted_certificate_authorities(const std::string& type,
                                      const std::string& /*hostname*/) override
         {
         std::vector<Botan::Certificate_Store*> v;

         // don't ask for client certs
         if(type == "tls-server")
            {
            return v;
            }

         for(auto const& cs : m_certstores)
            {
            v.push_back(cs.get());
            }

         return v;
         }

      std::vector<Botan::X509_Certificate> cert_chain(
         const std::vector<std::string>& algos,
         const std::string& type,
         const std::string& hostname) override
         {
         BOTAN_UNUSED(type);

         for(auto const& i : m_creds)
            {
            if(std::find(algos.begin(), algos.end(), i.key->algo_name()) == algos.end())
               {
               continue;
               }

            if(hostname != "" && !i.certs[0].matches_dns_name(hostname))
               {
               continue;
               }

            return i.certs;
            }

         return std::vector<Botan::X509_Certificate>();
         }

      Botan::Private_Key* private_key_for(const Botan::X509_Certificate& cert,
                                          const std::string& /*type*/,
                                          const std::string& /*context*/) override
         {
         for(auto const& i : m_creds)
            {
            if(cert == i.certs[0])
               {
               return i.key.get();
               }
            }

         return nullptr;
         }

   private:
      struct Certificate_Info
         {
         std::vector<Botan::X509_Certificate> certs;
         std::shared_ptr<Botan::Private_Key> key;
         };

      std::vector<Certificate_Info> m_creds;
      std::vector<std::shared_ptr<Botan::Certificate_Store>> m_certstores;
   };

#endif
